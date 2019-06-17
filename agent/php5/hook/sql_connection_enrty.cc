/*
 * Copyright 2017-2019 Baidu Inc.
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

#include "openrasp_sql.h"
#include "openrasp_hook.h"
#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "agent/shared_config_manager.h"

void SqlConnectionEntry::set_connection_string(std::string connection_string)
{
  this->connection_string = connection_string;
}
std::string SqlConnectionEntry::get_connection_string() const
{
  return connection_string;
}

void SqlConnectionEntry::set_server(std::string server)
{
  this->server = server;
}
std::string SqlConnectionEntry::get_server() const
{
  return server;
}

void SqlConnectionEntry::set_host(std::string host)
{
  this->host = host;
}

void SqlConnectionEntry::set_username(std::string username)
{
  this->username = username;
}
std::string SqlConnectionEntry::get_username() const
{
  return username;
}

void SqlConnectionEntry::set_password(std::string password)
{
  this->password = password;
}
std::string SqlConnectionEntry::get_password() const
{
  return password;
}

void SqlConnectionEntry::set_socket(std::string socket)
{
  this->socket = socket;
}

std::string SqlConnectionEntry::get_socket() const
{
  return socket;
}

void SqlConnectionEntry::set_port(int port)
{
  this->port = port;
}

std::string SqlConnectionEntry::build_policy_msg(connection_policy_type type)
{

  std::ostringstream oss;
  if (connection_policy_type::USER == type)
  {
    oss << "Database security - Connecting to a "
        << server
        << " instance using the high privileged account: "
        << username;
    if (get_using_socket())
    {
      oss << " (via unix domain socket)";
    }
  }
  else if (connection_policy_type::PASSWORD == type)
  {
    oss << "Database security baseline - weak password detected for \""
        << username
        << "\" account, password is: \""
        << password
        << "\"";
  }
  return oss.str();
}

std::string SqlConnectionEntry::get_type_name(SqlConnectionEntry::connection_policy_type type)
{
  switch (type)
  {
  case connection_policy_type::USER:
    return "user";
    break;
  case connection_policy_type::PASSWORD:
  default:
    return "password";
    break;
  }
}

long SqlConnectionEntry::get_type_id(SqlConnectionEntry::connection_policy_type type)
{
  switch (type)
  {
  case connection_policy_type::USER:
    return 3006;
    break;
  case connection_policy_type::PASSWORD:
  default:
    return 3003;
    break;
  }
}

bool SqlConnectionEntry::check_high_privileged()
{
  static const std::multimap<std::string, std::string> database_username_blacklists = {
      {"mysql", "root"},
      {"mssql", "sa"},
      {"pgsql", "postgres"},
      {"oci", "dbsnmp"},
      {"oci", "sysman"},
      {"oci", "system"},
      {"oci", "sys"}};
  if (!get_username().empty())
  {
    auto pos = database_username_blacklists.equal_range(get_server());
    while (pos.first != pos.second)
    {
      if (get_username() == pos.first->second)
      {
        return true;
      }
      pos.first++;
    }
  }
  return false;
}

bool SqlConnectionEntry::check_weak_password()
{
  return openrasp::scm->is_password_weak(get_password());
}

ulong SqlConnectionEntry::build_hash_code(connection_policy_type type)
{
  std::string server_host_port = server + "-" + host + ":" +
                                 (get_using_socket() ? socket : std::to_string(port)) +
                                 get_type_name(type);
  return zend_inline_hash_func(server_host_port.c_str(), server_host_port.length());
}

void SqlConnectionEntry::set_using_socket(bool using_socket)
{
  this->using_socket = using_socket;
}

bool SqlConnectionEntry::get_using_socket() const
{
  return using_socket;
}

void SqlConnectionEntry::set_name_value(const char *name, const char *val)
{
  if (strcmp(name, "user") == 0)
  {
    set_username(val);
  }
  else if (strcmp(name, "password") == 0)
  {
    set_password(val);
  }
  else if (strcmp(name, "host") == 0)
  {
    set_host(val);
    struct stat sb;
    if (stat(val, &sb) == 0)
    {
      set_using_socket((sb.st_mode & S_IFDIR) != 0 || (sb.st_mode & S_IFSOCK) != 0);
    }
    else
    {
      set_using_socket(false);
    }
  }
  else if (strcmp(name, "port") == 0)
  {
    set_port(atoi(val));
  }
}

void SqlConnectionEntry::connection_entry_policy_log(connection_policy_type type)
{
  zval *policy_array = nullptr;
  MAKE_STD_ZVAL(policy_array);
  array_init(policy_array);
  add_assoc_string(policy_array, "message", (char *)build_policy_msg(type).c_str(), 1);
  add_assoc_long(policy_array, "policy_id", get_type_id(type));
  zval *connection_params = nullptr;
  MAKE_STD_ZVAL(connection_params);
  array_init(connection_params);
  build_connection_params(connection_params, type);
  add_assoc_zval(policy_array, "policy_params", connection_params);
  TSRMLS_FETCH();
  LOG_G(policy_logger).log(LEVEL_INFO, policy_array TSRMLS_CC);
  zval_ptr_dtor(&policy_array);
}

void SqlConnectionEntry::append_host_port(const std::string &host, int port)
{
  this->host = host;
  this->port = port;
}

void SqlConnectionEntry::build_connection_params(zval *params, connection_policy_type type)
{
  if (params && Z_TYPE_P(params) == IS_ARRAY)
  {
    write_host_to_params(params);
    write_port_to_params(params);
    write_socket_to_params(params);
    add_assoc_string(params, "server", (char *)get_server().c_str(), 1);
    add_assoc_string(params, "username", (char *)get_username().c_str(), 1);
    add_assoc_string(params, "connectionString", (char *)get_connection_string().c_str(), 1);
    if (connection_policy_type::PASSWORD == type)
    {
      add_assoc_string(params, "password", (char *)get_password().c_str(), 1);
    }
  }
}

void SqlConnectionEntry::write_host_to_params(zval *params)
{
  if (params && Z_TYPE_P(params) == IS_ARRAY)
  {
    add_assoc_string(params, "hostname", (char *)host.c_str(), 1);
  }
}

void SqlConnectionEntry::write_port_to_params(zval *params)
{
  if (params && Z_TYPE_P(params) == IS_ARRAY)
  {
    add_assoc_long(params, "port", port);
  }
}

void SqlConnectionEntry::write_socket_to_params(zval *params)
{
  if (params && Z_TYPE_P(params) == IS_ARRAY)
  {
    add_assoc_string(params, "socket", (char *)socket.c_str(), 1);
  }
}

bool SqlConnectionEntry::parse(std::string uri)
{
  return true;
}
