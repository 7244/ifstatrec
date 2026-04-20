#include <WITCH/WITCH.h>
#include <WITCH/PR/PR.h>
#include <WITCH/T/T.h>
#include <WITCH/generic_alloc.h>

#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <csignal>
#include <print>

enum class data_point_code_t : uint8_t{
  valid,
  delayed
};

static std::string get_default_interface(){
  std::ifstream f("/proc/net/route");
  std::string line;
  std::string iface;
  while(std::getline(f, line)){
    std::istringstream iss(line);
    iss >> iface;
    std::string dst;
    iss >> dst;
    if(dst == "00000000"){
      return iface;
    }
  }

  __abort();
}

uintptr_t counter_type_count;
std::string_view *counter_type_name;

static data_point_code_t read_counters(const std::string& interface, uint64_t *counters){
  for(auto i = counter_type_count; i--;){
    /* TODO can be expensive */
    std::ifstream f("/sys/class/net/" + interface + "/statistics/" + std::string(counter_type_name[i]));
    f >> counters[i];
  }
  return data_point_code_t::valid;
}

static void diff_counters(uint64_t *counters0, uint64_t *counters1){
  for(auto i = counter_type_count; i--;){
    counters0[i] = counters1[i] - counters0[i];
  }
}

uintptr_t signal_came = false;
static void signal_handler(int){
  __atomic_store_n(&signal_came, true, __ATOMIC_SEQ_CST);
}

int main(){
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  auto ns_per = (uint64_t)1000000;
  auto inaccuracy_time_divide = (uint64_t)100;

  auto interface = get_default_interface();
  std::print("using interface: {}\n", interface);

  counter_type_count = 1;
  counter_type_name = (std::string_view*)__generic_mmap(counter_type_count * sizeof(std::string_view));
  if((uintptr_t)counter_type_name > (uintptr_t)-0x1000){
    __abort();
  }
  counter_type_name[0] = "rx_packets";

  uint64_t *counters2[2];
  {
    auto _counters2 = __generic_mmap(2 * counter_type_count * sizeof(uint64_t));
    if((uintptr_t)_counters2 > (uintptr_t)-0x1000){
      __abort();
    }
    counters2[0] = (uint64_t *)&((uint8_t*)_counters2)[0 * (counter_type_count * sizeof(uint64_t))];
    counters2[1] = (uint64_t *)&((uint8_t*)_counters2)[1 * (counter_type_count * sizeof(uint64_t))];
  }
  
  uintptr_t counter_flip = 0;

  std::print("press ctrl+c to stop and save\n");

  /* lets warm thread */
  {
    auto warm_thread = [&](){
      auto warm_start = T_nowi();
      while(1){
        /* shouldnt optimized away since it does file io. */
        read_counters(interface, counters2[counter_flip]);

        if((sint64_t)T_nowi() - (sint64_t)warm_start > 100'000'000){
          break;
        }

        /* funny that we warm it and relax same time. */
        __processor_relax();
      }
    };

    auto first_cpu = sched_getcpu();
    if(first_cpu < 0){
      __abort();
    }
  
    warm_thread();

    auto second_cpu = sched_getcpu();
    {
      if(second_cpu < 0){
        __abort();
      }
    
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(second_cpu, &set);
    
      if(sched_setaffinity(0, sizeof(cpu_set_t), &set) < 0){
        __abort();
      }
    }

    if(first_cpu != second_cpu){
      warm_thread();
    }
  }

  {
    auto err = read_counters(interface, counters2[counter_flip]);
    counter_flip ^= 1;
    if(err != data_point_code_t::valid){
      __abort();
    }
  }

  std::vector<uint8_t> data;
  data.reserve(1000000);

  auto wanted_time = T_nowi();

  bool next_is_failed = false;

  uint64_t total_points = 0;
  uint64_t total_failed_points = 0;
  while(!__atomic_load_n(&signal_came, __ATOMIC_SEQ_CST)){
    wanted_time += ns_per;

    auto wanted_time_early = wanted_time - ns_per / inaccuracy_time_divide;

    auto now = decltype(T_nowi()){};
    while(1){
      now = T_nowi();
      if(now >= wanted_time_early){
        break;
      }
      __processor_relax();
    }

    auto code = read_counters(interface, counters2[counter_flip]);
    counter_flip ^= 1;

    /* incase function takes too long time */
    now = T_nowi();

    auto diff = (sint64_t)now - (sint64_t)wanted_time;
    if(code != data_point_code_t::valid){
      next_is_failed = true;
    }
    else if(diff >= (sint64_t)ns_per / 100){
      next_is_failed = true;
      code = data_point_code_t::delayed;
    }
    else if(next_is_failed){
      next_is_failed = false;
      code = data_point_code_t::delayed;
    }

    data.emplace_back((uint8_t)code);

    if(code == data_point_code_t::valid){
      diff_counters(counters2[counter_flip], counters2[counter_flip ^ 1]);
      data.append_range(std::span((uint8_t*)counters2[counter_flip], counter_type_count * sizeof(uint64_t)));
    }
    else{
      total_failed_points += 1;
    }

    total_points += 1;
  }

  std::print("points:\n");
  std::print("  total: {}\n", total_points);
  std::print("  failed: {}\n", total_failed_points);
  std::print("total size: {}\n", data.size());

  {
    std::ofstream f("record.ifsr", std::ios::binary);
    uint64_t header[] = {
      counter_type_count,
      (uint64_t)ns_per,
      total_points,
      data.size(),
    };
    f.write((const char*)header, sizeof(header));
    f.write((const char*)data.data(), data.size());
  }

  std::print("done\n");

  return 0;
}
