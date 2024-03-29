/**
 * Copyright 2020 Hung-Hsin Chen, LSA Lab, National Tsing Hua University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * This is a per-GPU manager/scheduler.
 * Based on the information provided by clients, it decide which client to run
 * and give token to that client. This scheduler act as a daemon, accepting
 * connection and requests from pod manager or hook library directly.
 */

#include <iostream>
#include <fstream>
#include "scheduler.h"

#include <arpa/inet.h>
#include <errno.h>
#include <execinfo.h>
#include <getopt.h>
#include <linux/limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <limits>
#include <list>
#include <map>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

#include "debug.h"
#include "util.h"
#ifdef RANDOM_QUOTA
#include <random>
#endif

using std::string;
using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::steady_clock;

// signal handler
void sig_handler(int);
#ifdef _DEBUG
void dump_history(int);
#endif

// helper function for getting timespec
struct timespec get_timespec_after(double ms) {
  struct timespec ts;
  // now
  clock_gettime(CLOCK_MONOTONIC, &ts);

  double sec = ms / 1e3;
  ts.tv_sec += floor(sec);
  ts.tv_nsec += (sec - floor(sec)) * 1e9;
  ts.tv_sec += ts.tv_nsec / 1000000000;
  ts.tv_nsec %= 1000000000;
  return ts;
}

// all in milliseconds
double QUOTA = 250.0;
double MIN_QUOTA = 100.0;
double WINDOW_SIZE = 10000.0;
size_t g_sm_occupied = 0;
int verbosity = 0;
char* log_name = "/kubeshare/log/gemini-scheduler.log";
#define EVENT_SIZE sizeof(struct inotify_event)
#define BUF_LEN (1024 * (EVENT_SIZE + 16))
auto PROGRESS_START = steady_clock::now();
char limit_file_name[PATH_MAX] = "resource-config.txt";
char limit_file_dir[PATH_MAX] = ".";

std::list<History> history_list;
#ifdef _DEBUG
std::list<History> full_history;
#endif

// milliseconds since scheduler process started
inline double ms_since_start() {
  return duration_cast<microseconds>(steady_clock::now() - PROGRESS_START).count() / 1e3;
}

ClientInfo::ClientInfo(double baseq, double minq, double maxq, double minf, double maxf)
    : BASE_QUOTA(baseq), MIN_QUOTA(minq), MAX_QUOTA(maxq), MIN_FRAC(minf), MAX_FRAC(maxf) {
  quota_ = BASE_QUOTA;
  latest_overuse_ = 0.0;
  latest_actual_usage_ = 0.0;
  burst_ = 0.0;
};

ClientInfo::~ClientInfo(){};

void ClientInfo::set_burst(double estimated_burst) { burst_ = estimated_burst; }

void ClientInfo::update_return_time(double overuse) {
  double now = ms_since_start();
  for (auto it = history_list.rbegin(); it != history_list.rend(); it++) {
    if (it->name == this->name) {
      // client may not use up all of the allocated time
      it->end = std::min(now, it->end + overuse);
      latest_actual_usage_ = it->end - it->start;
      break;
    }
  }
  latest_overuse_ = overuse;
#ifdef _DEBUG
  for (auto it = full_history.rbegin(); it != full_history.rend(); it++) {
    if (it->name == this->name) {
      it->end = std::min(now, it->end + overuse);
      break;
    }
  }
#endif
}

void ClientInfo::Record(double quota) {
  History hist;
  hist.name = this->name;
  hist.start = ms_since_start();
  hist.end = hist.start + quota;
  history_list.push_back(hist);
#ifdef _DEBUG
  full_history.push_back(hist);
#endif
}

double ClientInfo::get_min_fraction() { return MIN_FRAC; }

double ClientInfo::get_max_fraction() { return MAX_FRAC; }

// self-adaptive quota algorithm
double ClientInfo::get_quota() {
  const double UPDATE_RATE = 0.5;  // how drastically will the quota changes

  if (burst_ < 1e-9) {
    // special case when no burst data available, just fallback to static quota
    quota_ = BASE_QUOTA;
    DEBUG(log_name, __FILE__, (long)__LINE__, "%s: fallback to static quota, assign quota: %.3fms", name.c_str(), quota_);
  } else {
    quota_ = burst_ * UPDATE_RATE + quota_ * (1 - UPDATE_RATE);
    quota_ = std::max(quota_, MIN_QUOTA);  // lowerbound
    quota_ = std::min(quota_, MAX_QUOTA);  // upperbound
    DEBUG(log_name, __FILE__, (long)__LINE__, "%s: burst: %.3fms, assign quota: %.3fms", name.c_str(), burst_, quota_);
  }
  return quota_;
}

// map container name to object
std::map<string, ClientInfo *> client_info_map;

std::list<candidate_t> candidates;
pthread_mutex_t candidate_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t candidate_cond;  // initialized with CLOCK_MONOTONIC in main()

void read_resource_config() {
  std::ifstream fin;
  ClientInfo *client_inf;
  char client_name[HOST_NAME_MAX], full_path[PATH_MAX];
  size_t gpu_memory_size, sm_partition;
  double gpu_min_fraction, gpu_max_fraction;
  int container_num;

  bzero(full_path, PATH_MAX);
  strncpy(full_path, limit_file_dir, PATH_MAX);
  if (limit_file_dir[strlen(limit_file_dir) - 1] != '/') full_path[strlen(limit_file_dir)] = '/';
  strncat(full_path, limit_file_name, PATH_MAX - strlen(full_path));

  // Read GPU limit usage
  fin.open(full_path, std::ios::in);
  if (!fin.is_open()) {
    ERROR(log_name, __FILE__, (long)__LINE__, "failed to open file %s: %s", full_path, strerror(errno));
    exit(1);
  }
  fin >> container_num;
  INFO(log_name, __FILE__, (long)__LINE__, "There are %d clients in the system...", container_num);
  for (int i = 0; i < container_num; i++) {
    fin >> client_name >> gpu_min_fraction >> gpu_max_fraction >> sm_partition >> gpu_memory_size;
    client_inf = new ClientInfo(QUOTA, MIN_QUOTA, gpu_min_fraction * WINDOW_SIZE, gpu_min_fraction,
                                gpu_max_fraction);
    client_inf->name = client_name;
    client_inf->gpu_sm_partition = sm_partition;
    client_inf->gpu_mem_limit = gpu_memory_size;
    if (client_info_map.find(client_name) != client_info_map.end())
      delete client_info_map[client_name];
    client_info_map[client_name] = client_inf;
    INFO(log_name, __FILE__, (long)__LINE__, "%s request: %.2f, limit: %.2f, memory limit: %lu bytes, sm_partition: %lu\%", client_name, gpu_min_fraction,
         gpu_max_fraction, gpu_memory_size, sm_partition);
  }
  fin.close();
}

void monitor_file(const char *path, const char *filename) {
  INFO(log_name, __FILE__, (long)__LINE__, "Monitor thread created.");
  int fd, wd;

  // Initialize Inotify
  fd = inotify_init();
  if (fd < 0) ERROR(log_name, __FILE__, (long)__LINE__, "Failed to initialize inotify");

  // add watch to starting directory
  wd = inotify_add_watch(fd, path, IN_CLOSE_WRITE);

  if (wd == -1)
    ERROR(log_name, __FILE__, (long)__LINE__, "Failed to add watch to '%s'.", path);
  else
    INFO(log_name, __FILE__, (long)__LINE__, "Watching '%s'.", path);

  // start watching
  while (1) {
    int i = 0;
    char buffer[BUF_LEN];
    bzero(buffer, BUF_LEN);
    int length = read(fd, buffer, BUF_LEN);
    if (length < 0) ERROR(log_name, __FILE__, (long)__LINE__, "Read error");

    while (i < length) {
      struct inotify_event *event = (struct inotify_event *)&buffer[i];

      if (event->len) {
        if (event->mask & IN_CLOSE_WRITE) {
          INFO(log_name, __FILE__, (long)__LINE__, "File %s modified with watch descriptor %d.", (const char *)event->name, event->wd);
          // if event is triggered by target file
          if (strcmp((const char *)event->name, filename) == 0) {
            INFO(log_name, __FILE__, (long)__LINE__, "Update containers' settings...");
            read_resource_config();
          }
        }
      }
      // update index to start of next event
      i += EVENT_SIZE + event->len;
    }
  }

  // Clean up
  // Supposed to be unreached.
  inotify_rm_watch(fd, wd);
  close(fd);
}

/**
 * Select a candidate whose current usage is less than its limit.
 * If no such candidates, calculate the time until time window content changes and sleep until then,
 * or until another candidate comes. If more than one candidate meets the requirement, select one
 * according to scheduling policy.
 * @return selected candidate
 */
std::vector<candidate_t> select_candidates() {
  while (true) {
    /* update history list and get usage in a time interval */
    double window_size = WINDOW_SIZE;
    int overlap_cnt = 0, i, j, k;
    std::map<string, double> usage;

    // instances to check for overlap
    std::vector<string> overlap_check_vector;

    double now = ms_since_start();
    double window_start = now - WINDOW_SIZE;
    double current_time;
    current_time = window_start;
    if (window_start < 0) {
      // elapsed time less than a window size
      window_size = now;
    }

    auto beyond_window = [=](const History &h) -> bool { return h.end < window_start; };
    history_list.remove_if(beyond_window);
    for (auto h : history_list) {
      usage[h.name] += h.end - std::max(h.start, current_time);
      if (verbosity > 1) {
        printf("{'container': '%s', 'start': %.3f, 'end': %.3f},\n", h.name.c_str(), h.start / 1e3,
               h.end / 1e3);
      }
    }

    /* select the candidate to give token */

    // no need for quick exit if the first one in candidates does not use GPU recently
    // as it's better to return a valid candidate set directly

    // sort by time
    /* select the ones to execute */
    std::vector<valid_candidate_t> vaild_candidates;

    pthread_mutex_lock(&candidate_mutex);
    double waittime = 2000; //2s
    for (auto it = candidates.begin(); it != candidates.end(); it++) {
      string name = it->name;
      double limit, require, missing, remaining;
      limit = client_info_map[name]->get_max_fraction() * window_size;
      require = client_info_map[name]->get_min_fraction() * window_size;
      missing = require - usage[name];
      remaining = limit - usage[name];

      if (remaining > 0)
        vaild_candidates.push_back({missing, remaining, usage[it->name], it->arrived_time, it});
      else
	waittime = std::min(waittime, -remaining);
    }
    pthread_mutex_unlock(&candidate_mutex);
    DEBUG(log_name, __FILE__, (long)__LINE__, "current valid candidates' size:%d", vaild_candidates.size());

    if (vaild_candidates.size() == 0) {
      // all candidates reach usage limit
      auto ts = get_timespec_after(waittime);
      DEBUG(log_name, __FILE__, (long)__LINE__, "sleep time %d ms", waittime); 
      // also wakes up if new requests come in
      pthread_mutex_lock(&candidate_mutex);
      pthread_cond_timedwait(&candidate_cond, &candidate_mutex, &ts);
      pthread_mutex_unlock(&candidate_mutex);
      continue;  // go to begin of loop
    }

    std::sort(vaild_candidates.begin(), vaild_candidates.end(), schd_priority);
    /* iterate candidates and sum up all the used sm */
    std::vector<candidate_t> approved_candidates;
    for (auto it = vaild_candidates.begin(); it != vaild_candidates.end(); it++) {
      string name = it->iter->name;
      size_t sm_partition = client_info_map[name]->gpu_sm_partition;
      if (g_sm_occupied + sm_partition <= SM_GLOBAL_LIMIT){
        approved_candidates.push_back(*(it->iter));
        pthread_mutex_lock(&candidate_mutex);
	candidates.erase(it->iter);
        pthread_mutex_unlock(&candidate_mutex);
      }
    }
    if (approved_candidates.size() == 0) {
      // all candidates reach usage limit
      auto ts = get_timespec_after(history_list.begin()->end - window_start);
      DEBUG(log_name, __FILE__, (long)__LINE__, "no approved candidates, sleep until %ld.%03ld", ts.tv_sec, ts.tv_nsec / 1000000);
      // also wakes up if new requests come in
      pthread_mutex_lock(&candidate_mutex);
      pthread_cond_timedwait(&candidate_cond, &candidate_mutex, &ts);
      pthread_mutex_unlock(&candidate_mutex);
      continue;  // go to begin of loop
    }
    return approved_candidates;
  }
}

// Get the information from message
void handle_message(int client_sock, char *message) {
  reqid_t req_id;  // simply pass this req_id back to Pod manager
  comm_request_t req;
  size_t hostname_len, offset = 0;
  char sbuf[RSP_MSG_LEN];
  char *attached, *client_name;
  ClientInfo *client_inf;
  attached = parse_request(message, &client_name, &hostname_len, &req_id, &req);

  if (client_info_map.find(string(client_name)) == client_info_map.end()) {
    WARNING(log_name, __FILE__, (long)__LINE__, "Unknown client \"%s\". Ignore this request.", client_name);
    return;
  }
  client_inf = client_info_map[string(client_name)];
  bzero(sbuf, RSP_MSG_LEN);
  int rc ,  MAX_RETRY = 5;
  if (req == REQ_QUOTA) {
    double overuse, burst, window;
    overuse = get_msg_data<double>(attached, offset);
    burst = get_msg_data<double>(attached, offset);

    client_inf->update_return_time(overuse);
    client_inf->set_burst(burst);
    pthread_mutex_lock(&candidate_mutex);
    candidates.push_back({client_sock, string(client_name), req_id, ms_since_start(), -1});
    pthread_cond_signal(&candidate_cond);  // wake schedule_daemon_func up
    pthread_mutex_unlock(&candidate_mutex);
    // select_candidate() will give quota later

  } else if (req == REQ_MEM_LIMIT) {

    prepare_response(sbuf, REQ_MEM_LIMIT, req_id, (size_t)client_inf->gpu_mem_used, client_inf->gpu_mem_limit);
    
     rc = multiple_attempt(
        [&]() -> int {
          if(send(client_sock, sbuf, RSP_MSG_LEN, 0) == -1) return -1;
          DEBUG(log_name, __FILE__, (long)__LINE__, "%s handle_message: REQ_MEM_LIMIT %d ",client_name, req_id);
        },
        MAX_RETRY, 3);
    
  } else if (req == REQ_MEM_UPDATE) {
    // ***for communication interface compatibility only***
    // memory usage is only tracked on hook library side
    DEBUG(log_name, __FILE__, (long)__LINE__, "scheduler always returns true for memory usage update!");

    size_t bytes;
    int is_allocate;
    bytes = get_msg_data<size_t>(attached, offset);
    is_allocate = get_msg_data<int>(attached, offset);

    int verdict=0;
    if (bytes>=0 && (is_allocate == 0 && client_inf->gpu_mem_used > bytes || is_allocate !=0 && client_inf->gpu_mem_used + bytes <= client_inf->gpu_mem_limit)){
      if (is_allocate!=0)
        client_inf->gpu_mem_used += bytes;
      else
        client_inf->gpu_mem_used -= bytes;
      verdict = 1;
    }

    prepare_response(sbuf, REQ_MEM_UPDATE, req_id, verdict);
    
    rc = multiple_attempt(
        [&]() -> int {
          if(send(client_sock, sbuf, RSP_MSG_LEN, 0) == -1) return -1;
          DEBUG(log_name, __FILE__, (long)__LINE__, "%s handle_message: REQ_MEM_UPDATE %d ",client_name, req_id);
        },
        MAX_RETRY, 3);
    
  } else {
    WARNING(log_name, __FILE__, (long)__LINE__, "\"%s\" send an unknown request.", client_name);
  }
}

//check and clear expired tokens
//if delivered tokens are zero, then there is no nearest waiting data
//if a token expired, update info and schedule another round
//else, we need to wait one of the token expires
std::list<candidate_t> tokenTakers;
std::list<candidate_t>::iterator min_tokenp;
bool operator <(const timespec& lhs, const timespec& rhs)
{
    if (lhs.tv_sec == rhs.tv_sec)
        return lhs.tv_nsec < rhs.tv_nsec;
    else
        return lhs.tv_sec < rhs.tv_sec;
}
bool update_tokens(){
  bool should_wait = true;  //by default, the valid candidate are all delivered with its quota
  auto now = ms_since_start();
  if (tokenTakers.size()==0) should_wait=false; // should not wait based on the running kernel, but pending directly
  else {
    DEBUG(log_name, __FILE__, (long)__LINE__, "tokenTaker not empty with size %d", tokenTakers.size());
    auto iter = tokenTakers.begin();
    while(iter!=tokenTakers.end()){
        if(iter->expired_time <= now){ //expired
            DEBUG(log_name, __FILE__, (long)__LINE__, "%s expired its token, update.", iter->name);
            g_sm_occupied -= client_info_map[iter->name]->gpu_sm_partition; 
            tokenTakers.erase(iter++);
            should_wait = false; //quick way to schedule another round
        }else{
            DEBUG(log_name, __FILE__, (long)__LINE__, "%s is still holding its token with quota %f", iter->name.c_str(), iter->expired_time-now);
            iter++;
        }
    } 
    min_tokenp = std::min_element(tokenTakers.begin(), tokenTakers.end(), 
		    [](const candidate_t& pairA, const candidate_t& pairB)->bool{return pairA.expired_time<pairB.expired_time;}
		    ); 
  }
  DEBUG(log_name, __FILE__, (long)__LINE__, "Current total partition: %d", g_sm_occupied);
  return should_wait;
};

bool remove_ifexists(string name){
    auto iter = tokenTakers.begin();
    while(iter != tokenTakers.end()){
       if(iter->name == name){
         DEBUG(log_name, __FILE__, (long)__LINE__, "the candidate %s returns early", iter->name.c_str());
	 g_sm_occupied -= client_info_map[iter->name]->gpu_sm_partition;
         tokenTakers.erase(iter); 
	 return true;
       }
       iter++;
    }
    return false;
}
 
void *schedule_daemon_func(void *) {
#ifdef RANDOM_QUOTA
  std::random_device rd;
  std::default_random_engine gen(rd());
  std::uniform_real_distribution<double> dis(0.4, 1.0);
#endif
  double quota;
  size_t sm_partition;
  //std::unordered_map<string, bool> scheduler_table;//@todo: remove evicted pod

  while (1) {
    pthread_mutex_lock(&candidate_mutex);
    if (candidates.size() != 0) {
      pthread_mutex_unlock(&candidate_mutex);
      // remove an entry from candidates
      update_tokens();//release the token to update sm info
      auto selects = select_candidates();
      for (auto selected: selects){
        DEBUG(log_name, __FILE__, (long)__LINE__, "select %s, waiting time: %.3f ms", selected.name.c_str(),
              ms_since_start() - selected.arrived_time);

        quota = client_info_map[selected.name]->get_quota();
        sm_partition = client_info_map[selected.name]->gpu_sm_partition;
#ifdef  RANDOM_QUOTA
        quota *= dis(gen);
#endif 
        client_info_map[selected.name]->Record(quota);

        // send quota to selected instance
        char sbuf[RSP_MSG_LEN];
        bzero(sbuf, RSP_MSG_LEN);
        prepare_response(sbuf, REQ_QUOTA, selected.req_id, quota);

        int rc, MAX_RETRY=5;
        rc = multiple_attempt(
          [&]() -> int {
            if(send(selected.socket, sbuf, RSP_MSG_LEN, 0) == -1){
                DEBUG(log_name, __FILE__, (long)__LINE__, "%s schedule_daemon_func - send error %s", selected.name.c_str(), strerror(errno));
               return -1;
            }
          },
          MAX_RETRY, 3);
    
	selected.expired_time = ms_since_start() + quota;
        g_sm_occupied += client_info_map[selected.name]->gpu_sm_partition;
	tokenTakers.emplace_back(selected);
      }

      bool should_wait = update_tokens();

      // wait until the selected one's quota time out
      pthread_mutex_lock(&candidate_mutex);
      DEBUG(log_name, __FILE__, (long)__LINE__, "current token lists' size:%d", tokenTakers.size());
      while (should_wait) {
	auto now = ms_since_start();
	double duration_ts = 0.0;
	if (min_tokenp->expired_time > now)
          duration_ts = min_tokenp->expired_time - now;
        auto wakeupTime = get_timespec_after(duration_ts);
        DEBUG(log_name, __FILE__, (long)__LINE__, "waiting %f ms as we should wait", duration_ts);
        int rc = pthread_cond_timedwait(&candidate_cond, &candidate_mutex, &(wakeupTime));
	//just wait at then; in most cases, it's ok
        if (rc == ETIMEDOUT) {
          DEBUG(log_name, __FILE__, (long)__LINE__, "the candidate %s didn't return on time with size:%d", min_tokenp->name.c_str(), tokenTakers.size());
          should_wait = false;
          g_sm_occupied -= client_info_map[min_tokenp->name]->gpu_sm_partition;
	  tokenTakers.erase(min_tokenp);
        } else {
          //ignore new incoming request except it returns fast or its partition fits current remaining resources 
	  for (auto conn : candidates) {
          DEBUG(log_name, __FILE__, (long)__LINE__, "the candidate %s is comming", conn.name.c_str());
            if (remove_ifexists(conn.name) || client_info_map[conn.name]->gpu_sm_partition + g_sm_occupied <= SM_GLOBAL_LIMIT) {
               DEBUG(log_name, __FILE__, (long)__LINE__, "quit early");
              should_wait = false;
              break;
            }
          }
          DEBUG(log_name, __FILE__, (long)__LINE__, "quit not early");
        }
      }
      DEBUG(log_name, __FILE__, (long)__LINE__, "continue next round");
      pthread_mutex_unlock(&candidate_mutex);
    } else {
      // wait for incoming connections
      DEBUG(log_name, __FILE__, (long)__LINE__, "no candidates");
      pthread_cond_wait(&candidate_cond, &candidate_mutex);
      pthread_mutex_unlock(&candidate_mutex);
    }
  }
}

// daemon function for Pod manager: waiting for incoming request
void *pod_client_func(void *args) {
  int pod_sockfd = *((int *)args);
  char *rbuf = new char[REQ_MSG_LEN];
  ssize_t recv_rc;
  bzero(rbuf, REQ_MSG_LEN);

  while ((recv_rc = recv(pod_sockfd, rbuf, REQ_MSG_LEN, 0)) > 0) {
    DEBUG(log_name, __FILE__, (long)__LINE__, "pod_client_func recv -> handle message");
    handle_message(pod_sockfd, rbuf);
  }
  DEBUG(log_name, __FILE__, (long)__LINE__, "Connection closed by Pod manager. recv() returns %ld.", recv_rc);
  close(pod_sockfd);
  delete (int *)args;
  delete[] rbuf;
  pthread_exit(NULL);
}



int main(int argc, char *argv[]) {
  
  uint16_t schd_port = 50051;
  // parse command line options
  const char *optstring = "P:q:m:w:f:p:v:h";
  struct option opts[] = {{"port", required_argument, nullptr, 'P'},
                          {"quota", required_argument, nullptr, 'q'},
                          {"min_quota", required_argument, nullptr, 'm'},
                          {"window", required_argument, nullptr, 'w'},
                          {"limit_file", required_argument, nullptr, 'f'},
                          {"limit_file_dir", required_argument, nullptr, 'p'},
                          {"verbose", required_argument, nullptr, 'v'},
                          {"help", no_argument, nullptr, 'h'},
                          {nullptr, 0, nullptr, 0}};
  int opt;
  while ((opt = getopt_long(argc, argv, optstring, opts, NULL)) != -1) {
    switch (opt) {
      case 'P':
        schd_port = strtoul(optarg, nullptr, 10);
        break;
      case 'q':
        QUOTA = atof(optarg);
        break;
      case 'm':
        MIN_QUOTA = atof(optarg);
        break;
      case 'w':
        WINDOW_SIZE = atof(optarg);
        break;
      case 'f':
        strncpy(limit_file_name, optarg, PATH_MAX - 1);
        break;
      case 'p':
        strncpy(limit_file_dir, optarg, PATH_MAX - 1);
        break;
      case 'v':
        verbosity = atoi(optarg);
        break;
      case 'h':
        printf("usage: %s [options]\n", argv[0]);
        puts("Options:");
        puts("    -P [PORT], --port [PORT]");
        puts("    -q [QUOTA], --quota [QUOTA]");
        puts("    -m [MIN_QUOTA], --min_quota [MIN_QUOTA]");
        puts("    -w [WINDOW_SIZE], --window [WINDOW_SIZE]");
        puts("    -f [LIMIT_FILE], --limit_file [LIMIT_FILE]");
        puts("    -p [LIMIT_FILE_DIR], --limit_file_dir [LIMIT_FILE_DIR]");
        puts("    -v [LEVEL], --verbose [LEVEL]");
        puts("    -h, --help");
        return 0;
      default:
        break;
    }
  }

  if (verbosity > 0) {
    printf("Scheduler settings:\n");
    printf("    %-20s %.3f ms\n", "default quota:", QUOTA);
    printf("    %-20s %.3f ms\n", "minimum quota:", MIN_QUOTA);
    printf("    %-20s %.3f ms\n", "time window:", WINDOW_SIZE);
  }

  // register signal handler for debugging
  signal(SIGSEGV, sig_handler);
#ifdef _DEBUG
  if (verbosity > 0) signal(SIGINT, dump_history);
#endif

  // read configuration file
  read_resource_config();

  int rc;
  int sockfd = 0;
  int forClientSockfd = 0;
  struct sockaddr_in clientInfo;
  int addrlen = sizeof(clientInfo);

  // create a monitored thread
  std::thread t1(monitor_file, std::ref(limit_file_dir), std::ref(limit_file_name));
  t1.detach();

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd == -1) {
    ERROR(log_name, __FILE__, (long)__LINE__, "Fail to create a socket!");
    exit(-1);
  }

  struct sockaddr_in serverInfo;
  bzero(&serverInfo, sizeof(serverInfo));

  serverInfo.sin_family = PF_INET;
  serverInfo.sin_addr.s_addr = INADDR_ANY;
  serverInfo.sin_port = htons(schd_port);
  if (bind(sockfd, (struct sockaddr *)&serverInfo, sizeof(serverInfo)) < 0) {
    ERROR(log_name, __FILE__, (long)__LINE__, "cannot bind port");
    exit(-1);
  }
  listen(sockfd, SOMAXCONN);

  pthread_t tid;

  // initialize candidate_cond with CLOCK_MONOTONIC
  pthread_condattr_t attr_monotonic_clock;
  pthread_condattr_init(&attr_monotonic_clock);
  pthread_condattr_setclock(&attr_monotonic_clock, CLOCK_MONOTONIC);
  pthread_cond_init(&candidate_cond, &attr_monotonic_clock);

  rc = pthread_create(&tid, NULL, schedule_daemon_func, NULL);
  
  if (rc != 0) {
    ERROR(log_name, __FILE__, (long)__LINE__, "Return code from pthread_create(): %d", rc);
    exit(rc);
  }
  pthread_detach(tid);
  INFO(log_name, __FILE__, (long)__LINE__, "Waiting for incoming connection");

  while (
      (forClientSockfd = accept(sockfd, (struct sockaddr *)&clientInfo, (socklen_t *)&addrlen))) {
    INFO(log_name, __FILE__, (long)__LINE__, "Received an incoming connection.");
    pthread_t tid;
    int *pod_sockfd = new int;
    *pod_sockfd = forClientSockfd;
    // create a thread to service this Pod manager
    pthread_create(&tid, NULL, pod_client_func, pod_sockfd);
    pthread_detach(tid);
  }
  if (forClientSockfd < 0) {
    ERROR(log_name, __FILE__, (long)__LINE__, "Accept failed");
    return 1;
  }
  return 0;
}

void sig_handler(int sig) {
  void *arr[10];
  size_t s;
  s = backtrace(arr, 10);
  ERROR(log_name, __FILE__, (long)__LINE__, "Received signal %d", sig);
  backtrace_symbols_fd(arr, s, STDERR_FILENO);
  exit(sig);
}

#ifdef _DEBUG
// write full history to a json file
void dump_history(int sig) {
  char filename[20];
  sprintf(filename, "%ld.json", time(NULL));
  FILE *f = fopen(filename, "w");
  fputs("[\n", f);
  for (auto it = full_history.begin(); it != full_history.end(); it++) {
    fprintf(f, "\t{\"container\": \"%s\", \"start\": %.3lf, \"end\" : %.3lf}", it->name.c_str(),
            it->start / 1000.0, it->end / 1000.0);
    if (std::next(it) == full_history.end())
      fprintf(f, "\n");
    else
      fprintf(f, ",\n");
  }
  fputs("]\n", f);
  fclose(f);

  INFO(log_name, __FILE__, (long)__LINE__, "history dumped to %s", filename);
  exit(0);
}
#endif
