// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in 
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.


/*
 * longlink_packer.cc
 *
 *  Created on: 2012-7-18
 *      Author: yerungui, caoshaokun
 */

#include "longlink_packer.h"

#ifdef __APPLE__
#include "mars/comm/autobuffer.h"
#include "mars/xlog/xlogger.h"
#else
#include "comm/autobuffer.h"
#include "comm/xlogger/xlogger.h"
#include "comm/socket/unix_socket.h"
#endif
#define NOOP_CMDID 6
#define SIGNALKEEP_CMDID 243
#define PUSH_DATA_TASKID 0
#define INVALID_TRANSLATED_LOGIN_SEQ 0xFFFFFFFE

static uint32_t sg_client_version = 0;
static uint32_t sg_long_link_type = 0;
static const uint32_t LONG_LINK_DEFAULT = 0;
static const uint32_t LONG_LINK_MACS = 1;
static const uint32_t LONG_LINK_MQTT = 2;

static std::string TAG_EVENT_ID("11");
static uint32_t MACS_HEARTBEAT_SIZE = 9;
static BYTE MACS_HEARTBEAT_PACKET[9] = {'5', '=', '3', '3', 0, '3', '=', '2', 0};
static BYTE MACS_HEARTBEAT_PACKET_ANSWER[9] = {'5', '=', '3', '3', 0, '3', '=', '3', 0};
static uint32_t macs_translated_login_seq = INVALID_TRANSLATED_LOGIN_SEQ;
static const uint32_t kLongLinkIdentifyCheckerTaskID = 0xFFFFFFFE;

static const uint32_t MQTT_CONNECT = 1;
static const uint32_t MQTT_CONNACK = 2;
static const uint32_t MQTT_PUBLISH = 3;
static const uint32_t MQTT_PUBACK = 4;
static const uint32_t MQTT_PUBREC = 5;
static const uint32_t MQTT_PUBREL = 6;
static const uint32_t MQTT_PUBCOMP = 7;
static const uint32_t MQTT_SUBSCRIBE = 8;
static const uint32_t MQTT_SUBACK = 9;
static const uint32_t MQTT_UNSUBSCRIBE = 10;
static const uint32_t MQTT_UNSUBACK = 11;
static const uint32_t MQTT_PINGREQ = 12;
static const uint32_t MQTT_PINGRESP = 13;
static const uint32_t MQTT_DISCONNECT = 14;
static const uint32_t MQTT_TYPEMAX = 15;
static const uint32_t MQTT_INVALIDATE = 0xFFFFFFFF;

static const BYTE MQTT_PACKET_TYPES[16] = {
    MQTT_INVALIDATE,
    (MQTT_CONNECT << 4),
    (MQTT_CONNACK << 4),
    (MQTT_PUBLISH << 4),
    (MQTT_PUBACK << 4),
    (MQTT_PUBREC << 4),
    (MQTT_PUBREL << 4) + 0x02,
    (MQTT_PUBCOMP << 4),
    (MQTT_SUBSCRIBE << 4) + 0x02,
    (MQTT_SUBACK << 4),
    (MQTT_UNSUBSCRIBE << 4) + 0x02,
    (MQTT_UNSUBACK << 4),
    (MQTT_PINGREQ << 4),
    (MQTT_PINGRESP << 4),
    (MQTT_DISCONNECT << 4),
    MQTT_INVALIDATE,
};


#pragma pack(push, 1)
struct __STNetMsgXpHeader {
    uint32_t    head_length;
    uint32_t    client_version;
    uint32_t    cmdid;
    uint32_t    seq;
    uint32_t	body_length;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct __MACSPacketHeader {
    BYTE packet_length0;
    BYTE packet_length1;
    BYTE packet_length2;
    BYTE packet_length3;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct __MQTTPacketHeader {
    BYTE packet_type;
};
#pragma pack(pop)


namespace mars {
    namespace stn {
        void SetClientVersion(uint32_t _client_version)  {
            sg_client_version = _client_version;
        }

        void SetLonglinkType(uint32_t type) {
            xinfo2(TSF "SetLonglinkType, type = %_",type);
            sg_long_link_type = type;
        }
    }
}

static int byteArrayToInt_C(BYTE bts0,BYTE bts1,BYTE bts2,BYTE bts3) {
    int value1 = 0xFF;
    int value2 = 0XFF;
    int value3 = 0XFF;
    int value4 = 0XFF;

    value1 = (value1 & bts0) * (0xFFFFFF + 1);
    value2 = (value2 & bts1) * (0xFFFF + 1);
    value3 = (value3 & bts2) * (0xFF + 1);
    value4 = value4 & bts3;

    return value1 + value2 + value3 + value4;
}

static int checkT2Header(__MACSPacketHeader& header) {
    if (header.packet_length0 == ((header.packet_length1 ^ header.packet_length2) ^ header.packet_length3)) {
        header.packet_length0 = 0;
        return byteArrayToInt_C(header.packet_length0,header.packet_length1,header.packet_length2,header.packet_length3);// .ntohl(head, true);
    }
    return -1;
}

static int __unpack_test(const void* _packed, size_t _packed_len, uint32_t& _cmdid, uint32_t& _seq, size_t& _package_len, size_t& _body_len) {
    __STNetMsgXpHeader st = {0};
    if (_packed_len < sizeof(__STNetMsgXpHeader)) {
        _package_len = 0;
        _body_len = 0;
        return LONGLINK_UNPACK_CONTINUE;
    }

    memcpy(&st, _packed, sizeof(__STNetMsgXpHeader));

    uint32_t head_len = ntohl(st.head_length);
    uint32_t client_version = ntohl(st.client_version);
    if (client_version != sg_client_version) {
        _package_len = 0;
        _body_len = 0;
        return LONGLINK_UNPACK_FALSE;
    }
    _cmdid = ntohl(st.cmdid);
    _seq = ntohl(st.seq);
    _body_len = ntohl(st.body_length);
    _package_len = head_len + _body_len;

    if (_package_len > 1024*1024) { return LONGLINK_UNPACK_FALSE; }
    if (_package_len > _packed_len) { return LONGLINK_UNPACK_CONTINUE; }

    return LONGLINK_UNPACK_OK;
}

static int __unpack_test(const void* _packed, size_t _packed_len, size_t& _package_len, size_t& _body_len) {
    xgroup2_define(close_log);
    __MACSPacketHeader mp = {0};
    if (_packed_len < sizeof(__MACSPacketHeader)) {
        _package_len = 0;
        _body_len = 0;
        return LONGLINK_UNPACK_CONTINUE;
    }

    memcpy(&mp, _packed, sizeof(__MACSPacketHeader));

    int pl = checkT2Header(mp);
    xinfo2(TSF", checkT2Header:%_,%_,%_,%_; pl = %_", mp.packet_length0, mp.packet_length1,mp.packet_length2,mp.packet_length3,pl) >> close_log;

    xinfo2_if(pl == -1, TSF", checkT2Header:%_,%_,%_,%_; pl = %_", mp.packet_length0, mp.packet_length1,mp.packet_length2,mp.packet_length3,pl) >> close_log;

    if (pl > 50 * 1024 || pl < 0) {
        return LONGLINK_UNPACK_FALSE;
    }

    _body_len = pl;
    _package_len = sizeof(__MACSPacketHeader) + _body_len;

    if (_package_len > 1024*1024) { return LONGLINK_UNPACK_FALSE; }
    if (_package_len > _packed_len) { return LONGLINK_UNPACK_CONTINUE; }

    return LONGLINK_UNPACK_OK;
}

bool __write_mqtt_packet_type(uint32_t _cmdid, AutoBuffer& _packed) {
    if (_cmdid < MQTT_TYPEMAX && _cmdid > 0) {
        BYTE type = MQTT_PACKET_TYPES[_cmdid];
        _packed.Write(&type, sizeof(type));
        return true;
    }
    return false;
}

void __write_mqtt_packet_size(const size_t _raw_len, AutoBuffer& _packed) {
    size_t size = _raw_len;
    while(size > 0){
        BYTE byte = size % 128;
        size = size / 128;
        if (size > 0) {
            byte = byte | 128;
        }
        _packed.Write(&byte, sizeof(byte));
    }
}

void __default_longlink_pack(uint32_t _cmdid, uint32_t _seq, const void* _raw, size_t _raw_len, AutoBuffer& _packed) {
    __STNetMsgXpHeader st = {0};
    st.head_length = htonl(sizeof(__STNetMsgXpHeader));
    st.client_version = htonl(sg_client_version);
    st.cmdid = htonl(_cmdid);
    st.seq = htonl(_seq);
    st.body_length = htonl(_raw_len);

    _packed.AllocWrite(sizeof(__STNetMsgXpHeader) + _raw_len);
    _packed.Write(&st, sizeof(st));

    if (NULL != _raw) _packed.Write(_raw, _raw_len);

    _packed.Seek(0, AutoBuffer::ESeekStart);
}

void __macs_longlink_pack(uint32_t _cmdid, uint32_t _seq, const void* _raw, size_t _raw_len, AutoBuffer& _packed) {
    xgroup2_define(close_log);
    xinfo2(TSF", TEST######################################pack")>> close_log;
    __MACSPacketHeader mp = {0};
    mp.packet_length3 = (BYTE) (_raw_len & 0xff);
    mp.packet_length2 = (BYTE) (_raw_len >> 8 & 0xff);
    mp.packet_length1 = (BYTE) (_raw_len >> 16 & 0xff);
    mp.packet_length0 = (BYTE) (mp.packet_length1 ^ mp.packet_length2 ^ mp.packet_length3);

    _packed.AllocWrite(sizeof(__MACSPacketHeader) + _raw_len);
    _packed.Write(&mp, sizeof(mp));

    if (NULL != _raw) _packed.Write(_raw, _raw_len);

    _packed.Seek(0, AutoBuffer::ESeekStart);
    if (_seq == kLongLinkIdentifyCheckerTaskID)
        macs_translated_login_seq = _cmdid;
}

void __mqtt_longlink_pack(uint32_t _cmdid, uint32_t _seq, const void* _raw, size_t _raw_len, AutoBuffer& _packed) {
    xgroup2_define(close_log);
    xinfo2(TSF", TEST######################################pack")>> close_log;
    size_t size = 0;
    if (_raw_len > 0) {
        if (_raw_len < 128)
            size = 1;
        else if (_raw_len < 16384)
            size = 2;
        else if (_raw_len < 2097152)
            size = 3;
        else if (_raw_len < 268435456)
            size = 4;
        else
            return;
    }
    _packed.AllocWrite(sizeof(BYTE) + size + _raw_len);
    _packed.Write(&mp, sizeof(mp));
    __write_mqtt_packet_type(_cmdid,_packed);
    __write_mqtt_packet_size(_raw_len,_packed);

    if (NULL != _raw) _packed.Write(_raw, _raw_len);

    _packed.Seek(0, AutoBuffer::ESeekStart);
}

void longlink_pack(uint32_t _cmdid, uint32_t _seq, const void* _raw, size_t _raw_len, AutoBuffer& _packed) {
    switch (sg_long_link_type) {
        case LONG_LINK_MACS:
            __macs_longlink_pack(_cmdid,_seq,_raw,_raw_len,_packed);
            break;
        case LONG_LINK_MQTT:
            __mqtt_longlink_pack(_cmdid,_seq,_raw,_raw_len,_packed);
            break;
        default:
            __default_longlink_pack(_cmdid,_seq,_raw,_raw_len,_packed);
            break;
    }
}

static std::string* findTagName(BYTE* data, size_t offset) {
    size_t index;
    for (index = offset; index >= 1; index--) {
        if (data[index] == 0) {
            break;
        }
    }
    if (index != 0) {
        char* tmp = new char[offset - index - 1];
        memcpy(tmp, data+index+1, offset - index - 1);
        std::string* ret = new std::string(tmp,offset - index - 1);
        delete []tmp;
        return ret;
    } else {
        char* tmp = new char[offset];
        memcpy(tmp, data, offset);
        std::string* ret = new std::string(tmp,offset);
        delete []tmp;
        return ret;
    }
}

static size_t findEquIndex(BYTE* data, size_t offset, size_t total) {
    size_t index;
    for (index = offset; index < total; index++) {
        if (data[index] == 61) {
            return index;
        }
    }
    return -1;
}

static size_t findStrLen(BYTE* data, size_t offset, size_t total) {
    size_t index;
    for (index = offset + 1; index < total; index++) {
        if (data[index] == 0) {
            break;
        }
    }
    return index - offset -1;
}

static BYTE* findStrData(BYTE* data, size_t offset, size_t length) {
    if (data[offset + length] == 0) {
        BYTE* tmp = new BYTE[length - 1];
        memcpy(tmp, data + offset + 1 , length-1);
        return tmp;
    } else {
        BYTE* tmp = new BYTE[length];
        memcpy(tmp, data + offset + 1 , length);
        return tmp;
    }
}

static uint32_t __unpack_seq(void* _packed, size_t _packed_len) {
    xgroup2_define(close_log);
    size_t index = 0;
    BYTE *bt = new BYTE[_packed_len];
    memcpy(bt, _packed, _packed_len);
    xinfo2(TSF", memcpy; bt = %_, _packed_len=%_", bt,_packed_len) >> close_log;
    while (index < _packed_len) {
        index = findEquIndex(bt, index, _packed_len);
        xinfo2(TSF", findEquIndex; index = %_", index) >> close_log;
        std::string* tagName = findTagName(bt, index);
        xinfo2(TSF", findTagName; tagName = %_", *tagName) >> close_log;
        size_t len = findStrLen(bt, index, _packed_len);
        xinfo2(TSF", findStrLen; len = %_", len) >> close_log;
        if (TAG_EVENT_ID == *tagName) {
            BYTE * tmp = findStrData(bt, index, len);
            uint32_t n = 0;
            std::string prt("{");
            for (size_t i = 0; i<len ; i++) {
                n = n*10+tmp[i]-'0';
                prt.append(1,tmp[i]);
                prt.append(",");
            }
            prt.append("}");
            xinfo2(TSF", findStrData; tmp = %_", prt) >> close_log;
            delete tagName;
            return n;
        }
        delete tagName;
        index += len;
    }
    return PUSH_DATA_TASKID;
}


int __default_longlink_unpack(const AutoBuffer& _packed, uint32_t& _cmdid, uint32_t& _seq, size_t& _package_len, AutoBuffer& _body) {
    size_t body_len = 0;
    int ret = __unpack_test(_packed.Ptr(), _packed.Length(), _cmdid,  _seq, _package_len, body_len);
    if (LONGLINK_UNPACK_OK != ret) return ret;

    _body.Write(AutoBuffer::ESeekCur, _packed.Ptr(_package_len-body_len), body_len);

    return ret;
}

int __macs_longlink_unpack(const AutoBuffer& _packed, uint32_t& _cmdid, uint32_t& _seq, size_t& _package_len, AutoBuffer& _body) {


    xgroup2_define(close_log);
    xinfo2(TSF", TEST######################################unpack")>> close_log;
    size_t body_len = 0;
    int ret = __unpack_test(_packed.Ptr(), _packed.Length(), _package_len, body_len);

    if (LONGLINK_UNPACK_OK != ret) return ret;

    _body.Write(AutoBuffer::ESeekCur, _packed.Ptr(_package_len-body_len), body_len);

    _cmdid = 0;

    _seq = __unpack_seq(_body.Ptr(), body_len);

    if (_seq == macs_translated_login_seq) {
        _seq = kLongLinkIdentifyCheckerTaskID;
        macs_translated_login_seq = INVALID_TRANSLATED_LOGIN_SEQ;
    }
    xinfo2(TSF", __unpack_seq; seq = %_", _seq) >> close_log;

    return ret;
}

int __mqtt_longlink_unpack(const AutoBuffer& _packed, uint32_t& _cmdid, uint32_t& _seq, size_t& _package_len, AutoBuffer& _body) {


    xgroup2_define(close_log);
    xinfo2(TSF", TEST######################################unpack")>> close_log;
    size_t body_len = 0;
    int ret = __unpack_test(_packed.Ptr(), _packed.Length(), _package_len, body_len);

    if (LONGLINK_UNPACK_OK != ret) return ret;

    _body.Write(AutoBuffer::ESeekCur, _packed.Ptr(_package_len-body_len), body_len);

    _cmdid = 0;

    _seq = __unpack_seq(_body.Ptr(), body_len);

    if (_seq == macs_translated_login_seq) {
        _seq = kLongLinkIdentifyCheckerTaskID;
        macs_translated_login_seq = INVALID_TRANSLATED_LOGIN_SEQ;
    }
    xinfo2(TSF", __unpack_seq; seq = %_", _seq) >> close_log;

    return ret;
}

int longlink_unpack(const AutoBuffer& _packed, uint32_t& _cmdid, uint32_t& _seq, size_t& _package_len, AutoBuffer& _body) {
    switch (sg_long_link_type) {
        case LONG_LINK_MACS:
            return __macs_longlink_unpack(_packed,_cmdid,_seq,_package_len,_body);
        case LONG_LINK_MQTT:
            return __mqtt_longlink_unpack(_packed,_cmdid,_seq,_package_len,_body);
        default:
            return __default_longlink_unpack(_packed,_cmdid,_seq,_package_len,_body);
    }
}


uint32_t longlink_noop_cmdid() {
    return NOOP_CMDID;
}


uint32_t longlink_noop_resp_cmdid() {
    return NOOP_CMDID;
}

uint32_t signal_keep_cmdid() {
    return SIGNALKEEP_CMDID;
}

void longlink_noop_req_body(AutoBuffer& _body) {
    switch (sg_long_link_type) {
        case LONG_LINK_MACS:
            _body.Write(MACS_HEARTBEAT_PACKET,MACS_HEARTBEAT_SIZE);
            break;
        default:
            break;
    }
}
void longlink_noop_resp_body(AutoBuffer& _body) {
    //_body.write(MACS_HEARTBEAT_PACKET_ANSWER,MACS_HEARTBEAT_SIZE);
}

uint32_t longlink_noop_interval() {
    return 0;
}

bool longlink_complexconnect_need_verify() {
    return false;
}

bool is_push_data(uint32_t _cmdid, uint32_t _taskid) {
    return PUSH_DATA_TASKID == _taskid;
}
