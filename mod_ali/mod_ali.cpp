/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Miao Gang <233300787@qq.com>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Miao Gang <233300787@qq.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Miao Gang <233300787@qq.com>
 *
 * mod_ali.c -- Ali Interface
 *
 */

#include <switch.h>
#include <bitset>
#include <iostream>
#include <sstream>
#include <memory>
#include <string>
#include <fstream>
#include <nlsClient.h>
#include <nlsEvent.h>
#include <speechSynthesizerRequest.h>
#include <nlsCommonSdk/Token.h>

using namespace std;
using namespace AlibabaNlsCommon;

//using std::ofstream;
//using std::ios;

using AlibabaNls::NlsClient;
using AlibabaNls::NlsEvent;
using AlibabaNls::LogDebug;
using AlibabaNls::LogInfo;
using AlibabaNls::SpeechSynthesizerCallback;
using AlibabaNls::SpeechSynthesizerRequest;
using AlibabaNlsCommon::NlsToken;

/* module name */
#define MOD_ALI "ali"
/* module config file */
#define CONFIG_FILE "ali.conf"

static struct {
    char* app_key;
    char* access_key;
    char* key_secret;
    int thread_count;
    int cache_size;
    char cache_path[128];
} globals;

SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_app_key, globals.app_key);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_access_key, globals.access_key);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_key_secret, globals.key_secret);

SWITCH_MODULE_LOAD_FUNCTION(mod_ali_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ali_shutdown);
SWITCH_MODULE_DEFINITION(mod_ali, mod_ali_load, mod_ali_shutdown, NULL);

struct ali_data {
    char* app_key;
    char* access_key;
    char* key_secret;
    char* voice;
    char* format;
    int sample_rate;
    int volume;
    int speech_rate;
    int pitch_rate;
    char* voice_file;
    int voice_cursor;
    //switch_file_handle_t *fh;
    //switch_buffer_t *audio_buffer;
};

// 自定义事件回调参数
struct ParamCallBack {
    std::string binAudioFile;
    std::ofstream audioFile;
};

typedef struct ali_data ali_t;

static switch_status_t ali_do_config(void) {
    switch_xml_t cfg, xml, settings, param;
    // inital
    globals.thread_count = 4;
    globals.cache_size = 1600;
    globals.app_key = NULL;
    globals.access_key = NULL;
    globals.key_secret = NULL;
    switch_snprintf(globals.cache_path, sizeof(globals.cache_path), "%s%sali_file_cache", SWITCH_GLOBAL_dirs.storage_dir, SWITCH_PATH_SEPARATOR);
    
    if (!(xml = switch_xml_open_cfg(CONFIG_FILE, &cfg, NULL))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", CONFIG_FILE);
    } else {
        if ((settings = switch_xml_child(cfg, "settings"))) {
            for (param = switch_xml_child(settings, "param"); param; param = param->next) {
                char *var = (char *) switch_xml_attr_soft(param, "name");
                char *val = (char *) switch_xml_attr_soft(param, "value");

                if (!strcmp(var, "thread_count")) {
                    globals.thread_count = atoi(val);
                } else if (!strcmp(var, "cache_size")) {
                    globals.cache_size = atoi(val);
                } else if (!strcmp(var, "app_key")) {
                    set_global_app_key(val);
                } else if (!strcmp(var, "access_key")) {
                    set_global_access_key(val);
                } else if (!strcmp(var, "key_secret")) {
                    set_global_key_secret(val);
                } else if (!strcmp(var, "cache_path")) {
                    switch_snprintf(globals.cache_path, sizeof(globals.cache_path), "%s", val);
                }
            }
        }
        switch_xml_free(xml);
    }
    
    // create ali cache directry
    switch_dir_make_recursive(globals.cache_path, SWITCH_DEFAULT_DIR_PERMS, NULL);
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t ali_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels, switch_speech_flag_t *flags)
{
    ali_t *ali = (ali_t *) switch_core_alloc(sh->memory_pool, sizeof(*ali));
    ali->voice = switch_core_strdup(sh->memory_pool, voice_name);
    ali->sample_rate = rate;
    ali->volume = 50;
    ali->speech_rate = 0;
    ali->pitch_rate = 0;
    ali->format = switch_core_strdup(sh->memory_pool, "wav");
    ali->app_key = switch_core_strdup(sh->memory_pool, globals.app_key);
    ali->access_key = switch_core_strdup(sh->memory_pool, globals.access_key);
    ali->key_secret = switch_core_strdup(sh->memory_pool, globals.key_secret);
    sh->private_info = ali;
    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t ali_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
    return SWITCH_STATUS_SUCCESS;
}

static void on_started(NlsEvent* cbEvent, void* cbParam) {}

static void on_completed(NlsEvent* cbEvent, void* cbParam) {}

static void on_closed(NlsEvent* cbEvent, void* cbParam) {
    ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
    tmpParam->audioFile.close();
    delete tmpParam; 
}

static void on_failed(NlsEvent* cbEvent, void* cbParam) {
    // remove file
    ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
    switch_file_remove(tmpParam->binAudioFile.c_str(), NULL);
}

static void on_received(NlsEvent* cbEvent, void* cbParam) {
    ParamCallBack* tmpParam = (ParamCallBack*)cbParam;
    const vector<unsigned char>& data = cbEvent->getBinaryData();
    if (data.size() > 0) {
        tmpParam->audioFile.write((char*)&data[0], data.size());
        tmpParam->audioFile.flush();
    }
}

static string ali_get_token(const char* accessKey, const char* keySecret) {
    string token;
    NlsToken nlsTokenRequest;
    nlsTokenRequest.setAccessKeyId(accessKey);
    nlsTokenRequest.setKeySecret(keySecret);
    if (-1 != nlsTokenRequest.applyNlsToken()) {
        token = nlsTokenRequest.getToken();
    }
    return token;
}

static int ali_file_size(const char* fileName) {
    int size = 0; 
    FILE* file = fopen(fileName, "rb");
    if (file) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fclose(file);
    }
    return size;
}

static size_t ali_file_read(const char* fileName, int start, void* data, size_t len) {
    size_t read_size = 0; 
    FILE* file = fopen(fileName, "rb");
    if (file) {
        fseek(file, start, SEEK_SET);
        read_size = fread(data, 1, len, file);
        fclose(file);
    }
    return read_size;
}

static switch_status_t ali_cloud_tts(const char* appKey, const char* accessKey, const char* keySecret, const char* voice, int volume, const char* format, int speechRate, int pitchRate, int sampleRate, const char* text, const string& file) {
    SpeechSynthesizerRequest* request = NlsClient::getInstance()->createSynthesizerRequest();
    if (request) {
        ParamCallBack* cbParam = new ParamCallBack;
        cbParam->binAudioFile = file;
        cbParam->audioFile.open(cbParam->binAudioFile.c_str(), std::ios::binary | std::ios::out);
        // 设置音频合成结束回调函数
        request->setOnSynthesisCompleted(on_completed, cbParam);
        // 设置音频合成通道关闭回调函数
        request->setOnChannelClosed(on_closed, cbParam);
        // 设置异常失败回调函数
        request->setOnTaskFailed(on_failed, cbParam);
        // 设置文本音频数据接收回调函数
        request->setOnBinaryDataReceived(on_received, cbParam);
        // 设置AppKey, 必填参数
        request->setAppKey(appKey);
        // 设置账号校验token, 必填参数
        request->setToken(ali_get_token(accessKey, keySecret).c_str()); 
        // 发音人, 包含"xiaoyun", "ruoxi", "xiaogang"等. 可选参数, 默认是xiaoyun
        request->setVoice(voice);
        // 音量, 范围是0~100, 可选参数, 默认50
        request->setVolume(volume);
        // 设置音频数据编码格式, 可选参数, 目前支持pcm, opus. 默认是pcm
        request->setFormat(format);
        // 语速, 范围是-500~500, 可选参数, 默认是0
        request->setSpeechRate(speechRate);
        // 语调, 范围是-500~500, 可选参数, 默认是0
        request->setPitchRate(pitchRate);
        // 设置音频数据采样率, 可选参数, 目前支持16000, 8000. 默认是16000
        request->setSampleRate(sampleRate);
        // 转换文本
        request->setText(text);
        // start convert
        if (request->start() >= 0) {
            request->stop();
            NlsClient::getInstance()->releaseSynthesizerRequest(request);
            return SWITCH_STATUS_SUCCESS;
        }
    }
    
    return SWITCH_STATUS_FALSE;
}

static string ali_md5(const char* data) {
    char digest[SWITCH_MD5_DIGEST_STRING_SIZE] = { 0 };
    switch_md5_string(digest, (void *) data, strlen(data));
    return string(digest);
}

static string ali_to_string(int64_t value, int pattern = 10) {
    ostringstream os;
    switch (pattern) {
        case 2: {
            bitset<64> b(value);
            os << b;
            break;
        }
        case 16:
            os << std::hex << value;
            break;
        default:
            os << value;
            break;
    }
    return os.str();
}

// start tts
static switch_status_t ali_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
    ali_t *ali = (ali_t *) sh->private_info;
    // TODO start tts
    // 1. file name voice_md5(params)/md5(text)
    string params;
    params += ali->app_key;
    params += ali->access_key;
    params += ali->key_secret;
    params += ali->voice;
    params += ali->format;
    params += ali_to_string(ali->sample_rate);
    params += ali_to_string(ali->volume);
    params += ali_to_string(ali->speech_rate);
    params += ali_to_string(ali->pitch_rate);

    // create voice path
    string voice_path = globals.cache_path;
    voice_path += SWITCH_PATH_SEPARATOR;
    voice_path += ali->voice;
    voice_path += "_";
    voice_path += ali_md5(params.c_str());
    if (SWITCH_STATUS_SUCCESS != switch_directory_exists(voice_path.c_str(), sh->memory_pool)) {
        switch_dir_make(voice_path.c_str(), SWITCH_DEFAULT_DIR_PERMS, sh->memory_pool);
    }
    
    // check file exist
    string voice_file = voice_path;
    voice_file += SWITCH_PATH_SEPARATOR;
    voice_file += ali_md5(text);
    voice_file += ".wav";

    ali->voice_file = switch_core_strdup(sh->memory_pool, voice_file.c_str());
    ali->voice_cursor = 0;

    if (SWITCH_STATUS_SUCCESS != switch_file_exists(voice_file.c_str(), sh->memory_pool)) {
        return ali_cloud_tts(ali->app_key, ali->access_key, ali->key_secret, ali->voice, ali->volume, ali->format, ali->speech_rate, ali->pitch_rate, ali->sample_rate, text, voice_file);
    }
    
    return SWITCH_STATUS_SUCCESS;
}

// stop tts
static void ali_speech_flush_tts(switch_speech_handle_t *sh)
{
}

// read tts data
static switch_status_t ali_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
    ali_t *ali = (ali_t *) sh->private_info;
    assert(ali != NULL);
    if (ali_file_size(ali->voice_file) >= globals.cache_size) {
        size_t read_size = ali_file_read(ali->voice_file, ali->voice_cursor, data, *datalen);
        if (read_size == 0) {
            return SWITCH_STATUS_BREAK;
        }
        ali->voice_cursor += read_size;
    }
    /*
    if (ali->fh != NULL && switch_test_flag(ali->fh, SWITCH_FILE_OPEN)) {
        size_t my_datalen = *datalen / 2;
        if (switch_core_file_read(ali->fh, data, &my_datalen) != SWITCH_STATUS_SUCCESS) {
            if (switch_test_flag(ali->fh, SWITCH_FILE_OPEN)) {
                switch_core_file_close(ali->fh);
            }
            return SWITCH_STATUS_FALSE;
        }
        *datalen = my_datalen * 2;
        if (datalen == 0) {
            if (switch_test_flag(ali->fh, SWITCH_FILE_OPEN)) {
                switch_core_file_close(ali->fh);
            }
            return SWITCH_STATUS_BREAK;
        }
    } else {
        if (ali_file_size(ali->voice_file) >= globals.cache_size) {
            if (switch_core_file_open(ali->fh, ali->voice_file, 0, ali->sample_rate, SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT, NULL) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_ali ----- failed to open file: %s\n", ali->voice_file);
                return SWITCH_STATUS_FALSE;
            }
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "mod_ali ----- wait file: %s cache enough\n", ali->voice_file);
        }
    }
    */
    return SWITCH_STATUS_SUCCESS;
}

static void ali_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
    ali_t *ali = (ali_t *) sh->private_info;
    assert(ali != NULL);

    if (!strcasecmp(param, "app_key")) {
        ali->app_key = switch_core_strdup(sh->memory_pool, val);
    } else if (!strcasecmp(param, "access_key")) {
        ali->access_key = switch_core_strdup(sh->memory_pool, val);
    } else if (!strcasecmp(param, "key_secret")) {
        ali->key_secret = switch_core_strdup(sh->memory_pool, val);
    } else if (!strcasecmp(param, "voice")) {
        ali->voice = switch_core_strdup(sh->memory_pool, val);
    } else if (!strcasecmp(param, "format")) {
        ali->format = switch_core_strdup(sh->memory_pool, val);
    } else if (!strcasecmp(param, "sample_rate")) {
        ali->sample_rate = atoi(val);
    } else if (!strcasecmp(param, "volume")) {
        ali->volume = atoi(val);
    } else if (!strcasecmp(param, "speech_rate")) {
        ali->speech_rate = atoi(val);
    } else if (!strcasecmp(param, "pitch_rate")) {
        ali->pitch_rate = atoi(val);
    }
    // set params
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_ali ----- text param=%s, val=%s\n", param, val);
}

static void ali_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_ali ----- numeric param=%s, val=%d\n", param, val);
}

static void ali_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_ali ----- float param=%s, val=%f\n", param, val);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_ali_load)
{
    switch_speech_interface_t *speech_interface;

    /* connect my internal structure to the blank pointer passed to me */
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);
    speech_interface = (switch_speech_interface_t *)switch_loadable_module_create_interface(*module_interface, SWITCH_SPEECH_INTERFACE);
    speech_interface->interface_name = MOD_ALI;
    speech_interface->speech_open = ali_speech_open;
    speech_interface->speech_close = ali_speech_close;
    speech_interface->speech_feed_tts = ali_speech_feed_tts;
    speech_interface->speech_read_tts = ali_speech_read_tts;
    speech_interface->speech_flush_tts = ali_speech_flush_tts;
    speech_interface->speech_text_param_tts = ali_text_param_tts;
    speech_interface->speech_numeric_param_tts = ali_numeric_param_tts;
    speech_interface->speech_float_param_tts = ali_float_param_tts;

    // read config
    ali_do_config();
    // init
    NlsClient::getInstance()->setLogConfig("ali-speech", LogDebug);
    NlsClient::getInstance()->startWorkThread(globals.thread_count);

    /* indicate that the module should continue to be loaded */
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ali_shutdown)
{
    NlsClient::releaseInstance();
    switch_safe_free(globals.app_key);
    switch_safe_free(globals.access_key);
    switch_safe_free(globals.key_secret);
    return SWITCH_STATUS_UNLOAD;
}

