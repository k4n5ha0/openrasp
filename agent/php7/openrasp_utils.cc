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

#include "openrasp.h"
#include "openrasp_ini.h"
#include "openrasp_utils.h"
#include "openrasp_log.h"
#include "utils/debug_trace.h"
#include "utils/regex.h"

extern "C"
{
#include "php_ini.h"
#include "php_main.h"
#include "php_streams.h"
#include "zend_smart_str.h"
#include "ext/standard/url.h"
#include "ext/standard/file.h"
#include "ext/json/php_json.h"
#include "Zend/zend_builtin_functions.h"
}
#include <string>

using openrasp::DebugTrace;

static std::vector<DebugTrace> build_debug_trace(long limit)
{
    zval trace_arr;
    std::vector<DebugTrace> array;
    zend_fetch_debug_backtrace(&trace_arr, 0, 0, 0);
    if (Z_TYPE(trace_arr) == IS_ARRAY)
    {
        int i = 0;
        HashTable *hash_arr = Z_ARRVAL(trace_arr);
        zval *ele_value = NULL;
        ZEND_HASH_FOREACH_VAL(hash_arr, ele_value)
        {
            if (++i > limit)
            {
                break;
            }
            if (Z_TYPE_P(ele_value) != IS_ARRAY)
            {
                continue;
            }
            DebugTrace trace_item;
            zval *trace_ele;
            if ((trace_ele = zend_hash_str_find(Z_ARRVAL_P(ele_value), ZEND_STRL("file"))) != NULL &&
                Z_TYPE_P(trace_ele) == IS_STRING)
            {
                trace_item.set_file(Z_STRVAL_P(trace_ele));
            }
            if ((trace_ele = zend_hash_str_find(Z_ARRVAL_P(ele_value), ZEND_STRL("function"))) != NULL &&
                Z_TYPE_P(trace_ele) == IS_STRING)
            {
                trace_item.set_function(Z_STRVAL_P(trace_ele));
            }
            if ((trace_ele = zend_hash_str_find(Z_ARRVAL_P(ele_value), ZEND_STRL("line"))) != NULL &&
                Z_TYPE_P(trace_ele) == IS_LONG)
            {
                trace_item.set_line(Z_LVAL_P(trace_ele));
            }
            array.push_back(trace_item);
        }
        ZEND_HASH_FOREACH_END();
    }
    zval_dtor(&trace_arr);
    return array;
}

std::vector<std::string> format_source_code_arr()
{
    std::vector<DebugTrace> trace = build_debug_trace(OPENRASP_CONFIG(plugin.maxstack));
    std::vector<std::string> array;
    for (DebugTrace &item : trace)
    {
        array.push_back(item.get_source_code());
    }
    return array;
}

void format_source_code_arr(zval *source_code_arr)
{
    auto array = format_source_code_arr();
    for (auto &str : array)
    {
        add_next_index_stringl(source_code_arr, str.c_str(), str.length());
    }
}

std::vector<std::string> format_debug_backtrace_arr()
{
    std::vector<DebugTrace> trace = build_debug_trace(OPENRASP_CONFIG(plugin.maxstack));
    std::vector<std::string> array;
    for (DebugTrace &item : trace)
    {
        array.push_back(item.to_plugin_string());
    }
    return array;
}

int recursive_mkdir(const char *path, int len, int mode)
{
    struct stat sb;
    if (VCWD_STAT(path, &sb) == 0 && (sb.st_mode & S_IFDIR) != 0)
    {
        return 1;
    }
    char *dirname = estrndup(path, len);
    int dirlen = php_dirname(dirname, len);
    int rst = recursive_mkdir(dirname, dirlen, mode);
    efree(dirname);
    if (rst)
    {
#ifndef PHP_WIN32
        mode_t oldmask = umask(0);
        rst = VCWD_MKDIR(path, mode);
        umask(oldmask);
#else
        rst = VCWD_MKDIR(path, mode);
#endif
        if (rst == 0 || EEXIST == errno)
        {
            return 1;
        }
        openrasp_error(LEVEL_WARNING, RUNTIME_ERROR, _("Could not create directory '%s': %s"), path, strerror(errno));
    }
    return 0;
}

const char *fetch_url_scheme(const char *filename)
{
    if (nullptr == filename)
    {
        return nullptr;
    }
    const char *p;
    for (p = filename; isalnum((int)*p) || *p == '+' || *p == '-' || *p == '.'; p++)
        ;
    if ((*p == ':') && (p - filename > 1) && (p[1] == '/') && (p[2] == '/'))
    {
        return p;
    }
    return nullptr;
}

void openrasp_scandir(const std::string dir_abs, std::vector<std::string> &plugins, std::function<bool(const char *filename)> file_filter, bool use_abs_path)
{
    DIR *dir;
    std::string result;
    struct dirent *ent;
    if ((dir = opendir(dir_abs.c_str())) != NULL)
    {
        while ((ent = readdir(dir)) != NULL)
        {
            if (file_filter)
            {
                if (file_filter(ent->d_name))
                {
                    plugins.push_back(use_abs_path ? (dir_abs + std::string(1, DEFAULT_SLASH) + std::string(ent->d_name)) : std::string(ent->d_name));
                }
            }
        }
        closedir(dir);
    }
}

char *fetch_outmost_string_from_ht(HashTable *ht, const char *arKey)
{
    zval *origin_zv;
    if ((origin_zv = zend_hash_str_find(ht, arKey, strlen(arKey))) != nullptr &&
        Z_TYPE_P(origin_zv) == IS_STRING)
    {
        return Z_STRVAL_P(origin_zv);
    }
    return nullptr;
}

bool get_entire_file_content(const char *file, std::string &content)
{
    std::ifstream ifs(file, std::ifstream::in | std::ifstream::binary);
    if (ifs.is_open() && ifs.good())
    {
        content = {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
        return true;
    }
    return false;
}

std::string json_encode_from_zval(zval *value)
{
    smart_str buf_json = {0};
    php_json_encode(&buf_json, value, 0);
    smart_str_0(&buf_json);
    std::string result(ZSTR_VAL(buf_json.s));
    smart_str_free(&buf_json);
    return result;
}

zend_string *fetch_request_body(size_t max_len)
{
    php_stream *stream = php_stream_open_wrapper("php://input", "rb", 0, NULL);
    if (stream)
    {
        zend_string *buf = php_stream_copy_to_mem(stream, max_len, 0);
        php_stream_close(stream);
        if (buf)
        {
            return buf;
        }
    }
    return zend_string_init("", strlen(""), 0);
}

bool need_alloc_shm_current_sapi()
{
    static const char *supported_sapis[] = {
        "fpm-fcgi",
        "apache2handler",
        NULL};
    const char **sapi_name;
    if (sapi_module.name)
    {
        for (sapi_name = supported_sapis; *sapi_name; sapi_name++)
        {
            if (strcmp(sapi_module.name, *sapi_name) == 0)
            {
                return 1;
            }
        }
    }
    return 0;
}

std::string convert_to_header_key(char *key, size_t length)
{
    std::string result;
    if (nullptr == key)
    {
        return result;
    }
    if (strcmp("HTTP_CONTENT_TYPE", key) == 0 || strcmp("CONTENT_TYPE", key) == 0)
    {
        result = "content-type";
    }
    else if (strcmp("HTTP_CONTENT_LENGTH", key) == 0 || strcmp("CONTENT_LENGTH", key) == 0)
    {
        result = "content-length";
    }
    else if (strncmp(key, "HTTP_", 5) == 0)
    {
        std::string http_header(key + 5, length - 5);
        for (auto &ch : http_header)
        {
            if (ch == '_')
            {
                ch = '-';
            }
            else
            {
                ch = std::tolower(ch);
            }
        }
        result = http_header;
    }
    return result;
}

bool openrasp_parse_url(const std::string &origin_url, std::string &scheme, std::string &host, std::string &port)
{
    php_url *url = php_url_parse_ex(origin_url.c_str(), origin_url.length());
    if (url)
    {
        if (url->scheme)
        {
#if (PHP_MINOR_VERSION < 3)
            scheme = std::string(url->scheme);
#else
            scheme = std::string(url->scheme->val, url->scheme->len);
#endif
        }
        if (url->host)
        {
#if (PHP_MINOR_VERSION < 3)
            host = std::string(url->host);
#else
            host = std::string(url->host->val, url->host->len);
#endif
        }
        if (url->port)
        {
            port = std::to_string(url->port);
        }
        php_url_free(url);
        return true;
    }
    return false;
}

bool verify_remote_management_ini()
{
	if (openrasp_ini.remote_management_enable && need_alloc_shm_current_sapi())
	{
		if (nullptr == openrasp_ini.backend_url || strcmp(openrasp_ini.backend_url, "") == 0)
		{
			openrasp_error(LEVEL_WARNING, CONFIG_ERROR, _("openrasp.backend_url is required when remote management is enabled."));
			return false;
		}
		if (nullptr == openrasp_ini.app_id || strcmp(openrasp_ini.app_id, "") == 0)
		{
			openrasp_error(LEVEL_WARNING, CONFIG_ERROR, _("openrasp.app_id is required when remote management is enabled."));
			return false;
		}
		else
		{
			if (!openrasp::regex_match(openrasp_ini.app_id, "^[0-9a-fA-F]{40}$"))
			{
				openrasp_error(LEVEL_WARNING, CONFIG_ERROR, _("openrasp.app_id must be exactly 40 characters long."));
				return false;
			}
		}
		if (nullptr == openrasp_ini.app_secret || strcmp(openrasp_ini.app_secret, "") == 0)
		{
			openrasp_error(LEVEL_WARNING, CONFIG_ERROR, _("openrasp.app_secret is required when remote management is enabled."));
			return false;
		}
		else
		{
			if (!openrasp::regex_match(openrasp_ini.app_secret, "^[0-9a-zA-Z_-]{43,45}"))
			{
				openrasp_error(LEVEL_WARNING, CONFIG_ERROR, _("openrasp.app_secret configuration format is incorrect."));
				return false;
			}
		}
	}
	return true;
}

std::map<std::string, std::string> get_env_map()
{
    std::map<std::string, std::string> result;
    char **env;
    for (env = environ; env != NULL && *env != NULL; env++)
    {
        std::string item(*env);
        std::size_t found = item.find("=");
        if (found != std::string::npos)
        {
            std::string key = item.substr(0, found);
            std::string value = item.substr(found + 1);
            result.insert({key, value});
        }
    }
    return result;
}

std::string get_phpversion()
{
    std::string version;
    zval function_name, retval;
    ZVAL_STRING(&function_name, "phpversion");
    if (call_user_function(EG(function_table), nullptr, &function_name, &retval, 0, nullptr) == SUCCESS)
    {
        if (Z_TYPE(retval) == IS_STRING)
        {
            version = std::string(Z_STRVAL(retval), Z_STRLEN(retval));
        }
        zval_ptr_dtor(&retval);
    }
    zval_ptr_dtor(&function_name);
    return version;
}