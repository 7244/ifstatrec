#include <WITCH/WITCH.h>
#include <WITCH/PR/PR.h>
#include <WITCH/T/T.h>

#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <csignal>
#include <print>

std::string get_default_interface(){
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

inline uint64_t read_counters(const std::string& interface){
  uint64_t v;
  std::ifstream f("/sys/class/net/" + interface + "/statistics/rx_packets");
  f >> v;
  return v;
}

uintptr_t signal_came = false;
void signal_handler(int){
  __atomic_store_n(&signal_came, true, __ATOMIC_SEQ_CST);
}

int main(){
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  auto interface = get_default_interface();
  std::print("using interface: {}\n", interface);

  auto prev_value = read_counters(interface);

  std::vector<uint64_t> data;
  data.reserve(1000000);

  std::print("press ctrl+c to stop and save\n");

  auto ns_per = 1000000;
  auto wanted_time = T_nowi() + ns_per;

  bool next_is_failed = false;
  uint64_t sum_diff = 0;

  while(!__atomic_load_n(&signal_came, __ATOMIC_SEQ_CST)){
    wanted_time += ns_per;

    auto now = decltype(T_nowi()){};
    do{
      now = T_nowi();
    }while(now < wanted_time);

    auto current_value = read_counters(interface);

    /* incase function takes too long time */
    now = T_nowi();

    auto data_value = current_value - prev_value;
    prev_value = current_value;

    auto diff = now - wanted_time;
    sum_diff += diff;
    if(diff >= ns_per / 100){
      next_is_failed = true;
      data_value = -1;
    }
    else if(next_is_failed){
      next_is_failed = false;
      data_value = -1;
    }

    data.emplace_back(data_value);
  }

  std::print("writing json with {} data points...\n", data.size());

  std::string json;
  json = "[";
  for(uintptr_t i = 0; i < data.size(); i++){
    if(i){
      json += ',';
    }
    json += std::to_string(data[i]);
  }
  json += "]";

  std::ofstream f("record.json");
  f << json;

  std::print("done\n");

  return 0;
}
