//
//  ycsbc.cc
//  YCSB-C
//
//  Created by Jinglei Ren on 12/19/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <vector>

#include "core/client.h"
#include "core/core_workload.h"
#include "core/timer.h"
#include "core/utils.h"
#include "db/db_factory.h"

using namespace std;

void UsageMessage(const char *command);
bool StrStartWith(const char *str, const char *pre);
string ParseCommandLine(int argc, const char *argv[], utils::Properties &props);

struct MemDB : ycsbc::DB {
  std::vector<ycsbc::DB::KVPair> data;
  std::vector<ycsbc::Operation> ops;
  MemDB(size_t size) {
    data.reserve(size);
    ops.reserve(size);
  }

  int Insert(const std::string &table, const std::string &key,
             std::vector<ycsbc::DB::KVPair> &values) override {
    string value;
    // for (const auto &kv : values) {
    //   value.append(kv.second);
    // }
    data.emplace_back(key, std::move(value));
    ops.emplace_back(ycsbc::Operation::INSERT);

    return ycsbc::DB::kOK;
  }

  int Read(const std::string &table, const std::string &key,
           const std::vector<std::string> *fields,
           std::vector<ycsbc::DB::KVPair> &result) override {
    // Read logic for in-memory database
    data.emplace_back(key, string());
    ops.emplace_back(ycsbc::Operation::READ);

    return ycsbc::DB::kOK;
  }

  int Update(const std::string &table, const std::string &key,
             std::vector<ycsbc::DB::KVPair> &values) override {
    // // Update logic for in-memory database
    string value;
    // for (const auto &kv : values) {
    //   value.append(kv.second);
    // }
    data.emplace_back(key, value);
    ops.emplace_back(ycsbc::Operation::UPDATE);
    return ycsbc::DB::kOK;
  }

  int Scan(const std::string &table, const std::string &key, int record_count,
           const std::vector<std::string> *fields,
           std::vector<std::vector<ycsbc::DB::KVPair>> &result) override {
    // Scan logic for in-memory database
    data.emplace_back(key, to_string(record_count));
    ops.emplace_back(ycsbc::Operation::SCAN);
    return ycsbc::DB::kOK;
  }
  int Delete(const std::string &table, const std::string &key) override {
    // Delete logic for in-memory database
    data.emplace_back(key, string());
    ops.emplace_back(ycsbc::Operation::DELETE);
    return ycsbc::DB::kOK;
  }
  ~MemDB() override {
    // dump to mem dev
    ofstream fs("/mnt/memdb/db", ios::out | ios::trunc);
    if (!fs.is_open()) {
      cerr << "Failed to open memdb file for writing." << endl;
      return;
    }
    for (int i = 0; i < data.size(); ++i) {
      string op = ops[i] == ycsbc::Operation::INSERT   ? "INSERT"
                  : ops[i] == ycsbc::Operation::READ   ? "READ"
                  : ops[i] == ycsbc::Operation::UPDATE ? "UPDATE"
                  : ops[i] == ycsbc::Operation::SCAN   ? "SCAN"
                  : ops[i] == ycsbc::Operation::DELETE ? "DELETE"
                                                       : "UNKNOWN";
      fs << data[i].first << '\t' << op << '\n';
    }
  }
};

int DelegateClient(ycsbc::DB *db, ycsbc::CoreWorkload *wl, const int num_ops,
                   bool is_loading) {
  db->Init();
  ycsbc::Client client(*db, *wl);
  int oks = 0;
  for (int i = 0; i < num_ops; ++i) {
    if (is_loading) {
      oks += client.DoInsert();
    } else {
      oks += client.DoTransaction();
    }
  }
  db->Close();
  return oks;
}

int main(const int argc, const char *argv[]) {
  utils::Properties props;
  string file_name = ParseCommandLine(argc, argv, props);

  // ycsbc::DB *db = ycsbc::DBFactory::CreateDB(props);
  // if (!db) {
  //   cout << "Unknown database name " << props["dbname"] << endl;
  //   exit(0);
  // }
  int total_ops = stoi(props[ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY]);
  MemDB memdb(total_ops * 2);
  ycsbc::DB *db = &memdb;

  ycsbc::CoreWorkload wl;
  wl.Init(props);

  const int num_threads = stoi(props.GetProperty("threadcount", "1"));

  // Loads data
  vector<future<int>> actual_ops;
  for (int i = 0; i < num_threads; ++i) {
    actual_ops.emplace_back(async(launch::async, DelegateClient, db, &wl,
                                  total_ops / num_threads, true));
  }
  assert((int)actual_ops.size() == num_threads);

  int sum = 0;
  for (auto &n : actual_ops) {
    assert(n.valid());
    sum += n.get();
  }
  cerr << "# Loading records:\t" << sum << endl;

  // Peforms transactions
  actual_ops.clear();
  total_ops = stoi(props[ycsbc::CoreWorkload::OPERATION_COUNT_PROPERTY]);
  utils::Timer<double> timer;
  timer.Start();
  for (int i = 0; i < num_threads; ++i) {
    actual_ops.emplace_back(async(launch::async, DelegateClient, db, &wl,
                                  total_ops / num_threads, false));
  }
  assert((int)actual_ops.size() == num_threads);

  sum = 0;
  for (auto &n : actual_ops) {
    assert(n.valid());
    sum += n.get();
  }
  double duration = timer.End();
  cerr << "# Transaction throughput (KTPS)" << endl;
  cerr << props["dbname"] << '\t' << file_name << '\t' << num_threads << '\t';
  cerr << total_ops / duration / 1000 << endl;
}

string ParseCommandLine(int argc, const char *argv[],
                        utils::Properties &props) {
  int argindex = 1;
  string filename;
  while (argindex < argc && StrStartWith(argv[argindex], "-")) {
    if (strcmp(argv[argindex], "-threads") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("threadcount", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-db") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("dbname", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-host") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("host", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-port") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("port", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-slaves") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("slaves", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-P") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      filename.assign(argv[argindex]);
      ifstream input(argv[argindex]);
      try {
        props.Load(input);
      } catch (const string &message) {
        cout << message << endl;
        exit(0);
      }
      input.close();
      argindex++;
    } else {
      cout << "Unknown option '" << argv[argindex] << "'" << endl;
      exit(0);
    }
  }

  if (argindex == 1 || argindex != argc) {
    UsageMessage(argv[0]);
    exit(0);
  }

  return filename;
}

void UsageMessage(const char *command) {
  cout << "Usage: " << command << " [options]" << endl;
  cout << "Options:" << endl;
  cout << "  -threads n: execute using n threads (default: 1)" << endl;
  cout << "  -db dbname: specify the name of the DB to use (default: basic)"
       << endl;
  cout << "  -P propertyfile: load properties from the given file. Multiple "
          "files can"
       << endl;
  cout << "                   be specified, and will be processed in the "
          "order "
          "specified"
       << endl;
}

inline bool StrStartWith(const char *str, const char *pre) {
  return strncmp(str, pre, strlen(pre)) == 0;
}
