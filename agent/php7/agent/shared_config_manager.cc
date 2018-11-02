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

#include "shared_config_manager.h"
#include "shared_config_block.h"

namespace openrasp
{
std::unique_ptr<SharedConfigManager> scm = nullptr;

SharedConfigManager::SharedConfigManager()
    : shared_config_block(nullptr), rwlock(nullptr)
{
    meta_size = ROUNDUP(sizeof(pthread_rwlock_t), 1 << 3);
}

SharedConfigManager::~SharedConfigManager()
{
    if (rwlock != nullptr)
    {
        delete rwlock;
        rwlock = nullptr;
    }
}

int SharedConfigManager::get_check_type_white_bit_mask(std::string url)
{
    DoubleArrayTrie::result_pair_type result_pair[CHECK_TYPE_NR_ITEMS];
    DoubleArrayTrie dat;
    int white_bit_mask = NO_TYPE;
    if (rwlock != nullptr && rwlock->read_try_lock())
    {
        ReadUnLocker auto_unlocker(rwlock);
        dat.load_existing_array((void *)shared_config_block->get_check_type_white_array(), shared_config_block->get_white_array_size());
        size_t num = dat.prefix_search(url.c_str(), result_pair, sizeof(result_pair));
        if (num > 0)
        {
            for (size_t i = 0; i < num; ++i)
            {
                white_bit_mask |= result_pair[i].value;
            }
        }
    }
    return white_bit_mask;
}

bool SharedConfigManager::write_check_type_white_array_to_shm(const void *source, size_t num)
{
    if (rwlock != nullptr && rwlock->write_try_lock())
    {
        WriteUnLocker auto_unlocker(rwlock);
        shared_config_block->reset_white_array(source, num);
        return true;
    }
    return false;
}

bool SharedConfigManager::build_check_type_white_array(std::map<std::string, int> &url_mask_map)
{
    std::vector<std::string> urls;
    std::vector<int> values;
    for (auto iter = url_mask_map.begin(); iter != url_mask_map.end(); iter++)
    {
        urls.push_back(iter->first);
        values.push_back(iter->second);
    }
    DoubleArrayTrie::result_pair_type result_pair[CHECK_TYPE_NR_ITEMS];
    DoubleArrayTrie dat;
    dat.build(urls.size(), &urls, 0, &values);
    write_check_type_white_array_to_shm(dat.array(), dat.total_size());
}

bool SharedConfigManager::build_check_type_white_array(OpenraspConfig &openrasp_config)
{
    std::map<std::string, int> url_mask_map;
    bool all_type_white = openrasp_config.Get("hook.white.ALL", false);
    if (all_type_white)
    {
        url_mask_map[""] = ALL_TYPE;
    }
    else
    {
        for (auto map_iter : CheckTypeNameMap)
        {
            std::vector<std::string> urls;
            urls = openrasp_config.GetArray("hook.white." + std::string(map_iter.second), urls);
            for (auto vector_iter : urls)
            {
                std::string target_url = (vector_iter == "all") ? "" : vector_iter;
                int mask = map_iter.first;
                auto it = url_mask_map.find(target_url);
                if (it != url_mask_map.end())
                {
                    mask |= it->second;
                }
                url_mask_map[target_url] = mask;
            }
        }
    }
    build_check_type_white_array(url_mask_map);
}

long SharedConfigManager::get_config_last_update()
{
    if (rwlock != nullptr && rwlock->read_try_lock())
    {
        ReadUnLocker auto_unlocker(rwlock);
        return shared_config_block->get_config_update_time();
    }
    return 0;
}

bool SharedConfigManager::set_config_last_update(long config_update_timestamp)
{
    if (rwlock != nullptr && rwlock->write_try_lock())
    {
        WriteUnLocker auto_unlocker(rwlock);
        shared_config_block->set_config_update_time(config_update_timestamp);
        return true;
    }
    return false;
}

long SharedConfigManager::get_log_max_backup()
{
    if (rwlock != nullptr && rwlock->read_try_lock())
    {
        ReadUnLocker auto_unlocker(rwlock);
        return shared_config_block->get_log_max_backup();
    }
    return 0;
}

bool SharedConfigManager::set_log_max_backup(long log_max_backup)
{
    if (rwlock != nullptr && rwlock->write_try_lock())
    {
        WriteUnLocker auto_unlocker(rwlock);
        shared_config_block->set_log_max_backup(log_max_backup);
        return true;
    }
    return false;
}

bool SharedConfigManager::startup()
{
    size_t total_size = meta_size + sizeof(SharedConfigBlock);
    char *shm_block = BaseManager::sm.create(SHMEM_SEC_CONF_BLOCK, total_size);
    if (shm_block)
    {
        memset(shm_block, 0, total_size);
        rwlock = new ReadWriteLock((pthread_rwlock_t *)shm_block, LOCK_PROCESS);
        char *shm_config_block = shm_block + meta_size;
        shared_config_block = reinterpret_cast<SharedConfigBlock *>(shm_config_block);
        std::map<std::string, int> all_type_white{{"", ALL_TYPE}};
        build_check_type_white_array(all_type_white);
        initialized = true;
        return true;
    }
    return false;
}

bool SharedConfigManager::shutdown()
{
    if (initialized)
    {
        if (rwlock != nullptr)
        {
            delete rwlock;
            rwlock = nullptr;
        }
        BaseManager::sm.destroy(SHMEM_SEC_CONF_BLOCK);
        initialized = false;
    }
    return true;
}

} // namespace openrasp