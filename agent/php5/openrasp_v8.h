/*
 * Copyright 2017-2018 Baidu Inc.
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

#ifndef OPENRASP_V8_H
#define OPENRASP_V8_H

#include "openrasp.h"
#undef COMPILER // conflict with v8 defination
#include <v8.h>
#include <libplatform/libplatform.h>
#include <v8-platform.h>
#include <mutex>
#include <queue>
#include <vector>
#include <chrono>
#include <string>
#include <memory>
#include <iostream>
#include <condition_variable>
#include <thread>
#include <fstream>

namespace openrasp
{
#define LOG(msg) \
  std::cerr << msg << std::endl;

#define TRYCATCH() \
  v8::TryCatch try_catch

inline v8::Local<v8::String> NewV8String(v8::Isolate *isolate, const char *str, size_t len = -1)
{
  return v8::String::NewFromUtf8(isolate, str, v8::NewStringType::kNormal, len).ToLocalChecked();
}

inline v8::Local<v8::String> NewV8String(v8::Isolate *isolate, const std::string &str)
{
  return NewV8String(isolate, str.c_str(), str.length());
}

#define V8STRING_EX(string, type, length) \
  (v8::String::NewFromUtf8(isolate, string, type, length))

#define V8STRING_N(string) \
  V8STRING_EX(string, v8::NewStringType::kNormal, -1)

#define V8STRING_I(string) \
  V8STRING_EX(string, v8::NewStringType::kInternalized, -1)

class TimeoutTask : public v8::Task
{
public:
  TimeoutTask(v8::Isolate *_isolate, int _milliseconds = 100);
  void Run() override;
  std::timed_mutex &GetMtx();

private:
  v8::Isolate *isolate;
  std::chrono::time_point<std::chrono::high_resolution_clock> time_point;
  std::timed_mutex mtx;
};

class openrasp_v8_js_src
{
public:
  std::string filename;
  std::string source;
};

class Snapshot : public v8::StartupData
{
public:
  uint64_t timestamp = 0;
  intptr_t external_references[2] = {
      reinterpret_cast<intptr_t>(v8native_log),
      0,
  };
  Snapshot(const v8::StartupData &blob) : v8::StartupData(blob){};
  Snapshot(const char *data = nullptr, size_t raw_size = 0, uint64_t timestamp = 0);
  Snapshot(const std::string &path, uint64_t timestamp = 0);
  Snapshot(const std::string &config, const std::vector<openrasp_v8_js_src> &plugin_list);
  ~Snapshot() { delete[] data; };
  bool Save(const std::string &path) const; // check errno when return value is false
  bool IsOk() const { return data && raw_size; };
  bool IsExpired(uint64_t timestamp) const { return timestamp > this->timestamp; };

private:
  static void v8native_log(const v8::FunctionCallbackInfo<v8::Value> &info);
};

namespace RequestContext
{
v8::Local<v8::Object> New(v8::Isolate *isolate);
}

class Isolate : public v8::Isolate
{
public:
  class Data
  {
  public:
    v8::Isolate::CreateParams create_params;
    v8::Persistent<v8::Object> RASP;
    v8::Persistent<v8::Function> check;
    v8::Persistent<v8::Object> request_context;
    v8::Persistent<v8::String> key_action;
    v8::Persistent<v8::String> key_message;
    v8::Persistent<v8::String> key_name;
    v8::Persistent<v8::String> key_confidence;
    v8::Persistent<v8::Function> console_log;
    v8::Persistent<v8::Function> JSON_stringify;
    int action_hash_ignore = 0;
    int action_hash_log = 0;
    int action_hash_block = 0;
    uint64_t timestamp = 0;
  };

  static Isolate *New(Snapshot *snapshot_blob);
  Data *GetData();
  void Dispose();
  bool IsExpired(uint64_t timestamp);
};

class openrasp_v8_process_globals
{
public:
  Snapshot *snapshot_blob = nullptr;
  std::mutex mtx;
  bool is_initialized = false;
  v8::Platform *v8_platform = nullptr;
  std::string plugin_config;
  std::vector<openrasp_v8_js_src> plugin_src_list;
  long plugin_update_timestamp = 0;
};

extern openrasp_v8_process_globals process_globals;

bool init_platform(TSRMLS_D);
bool shutdown_platform(TSRMLS_D);
void v8error_to_stream(v8::Isolate *isolate, v8::TryCatch &try_catch, std::ostream &buf);
v8::Local<v8::Value> zval_to_v8val(zval *val, v8::Isolate *isolate TSRMLS_DC);
v8::MaybeLocal<v8::Script> compile_script(std::string _source, std::string _filename, int _line_offset = 0);
v8::MaybeLocal<v8::Value> exec_script(v8::Isolate *isolate, v8::Local<v8::Context> context,
                                      std::string _source, std::string _filename, int _line_offset = 0);
void alarm_info(Isolate *isolate, v8::Local<v8::String> type, v8::Local<v8::Object> params, v8::Local<v8::Object> result TSRMLS_DC);
bool openrasp_check(Isolate *isolate, v8::Local<v8::String> type, v8::Local<v8::Object> params TSRMLS_DC);
unsigned char openrasp_check(const char *c_type, zval *z_params TSRMLS_DC);
} // namespace openrasp

ZEND_BEGIN_MODULE_GLOBALS(openrasp_v8)
_zend_openrasp_v8_globals()
{
  isolate = nullptr;
  action_hash_ignore = 0;
  action_hash_log = 0;
  action_hash_block = 0;
  is_isolate_initialized = false;
  is_env_initialized = false;
  is_running = false;
  plugin_update_timestamp = 0;
}
openrasp::Isolate *isolate;
v8::Isolate::CreateParams create_params;
v8::Persistent<v8::Context> context;
v8::Persistent<v8::Object> RASP;
v8::Persistent<v8::Function> check;
v8::Persistent<v8::Object> request_context;
v8::Persistent<v8::String> key_action;
v8::Persistent<v8::String> key_message;
v8::Persistent<v8::String> key_name;
v8::Persistent<v8::String> key_confidence;
v8::Persistent<v8::Function> console_log;
v8::Persistent<v8::Function> JSON_stringify;
int action_hash_ignore;
int action_hash_log;
int action_hash_block;
bool is_isolate_initialized;
bool is_env_initialized;
bool is_running;
long plugin_update_timestamp;
ZEND_END_MODULE_GLOBALS(openrasp_v8)

ZEND_EXTERN_MODULE_GLOBALS(openrasp_v8)

PHP_GINIT_FUNCTION(openrasp_v8);
PHP_GSHUTDOWN_FUNCTION(openrasp_v8);
PHP_MINIT_FUNCTION(openrasp_v8);
PHP_MSHUTDOWN_FUNCTION(openrasp_v8);
PHP_RINIT_FUNCTION(openrasp_v8);

#ifdef ZTS
#define OPENRASP_V8_G(v) TSRMG(openrasp_v8_globals_id, zend_openrasp_v8_globals *, v)
#define OPENRASP_V8_GP() ((zend_openrasp_v8_globals *)(*((void ***)tsrm_ls))[TSRM_UNSHUFFLE_RSRC_ID(openrasp_v8_globals_id)])
#else
#define OPENRASP_V8_G(v) (openrasp_v8_globals.v)
#define OPENRASP_V8_GP() (&openrasp_v8_globals)
#endif

#endif /* OPENRASP_v8_H */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
