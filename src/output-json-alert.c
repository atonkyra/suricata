/* Copyright (C) 2013-2014 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Tom DeCanio <td@npulsetech.com>
 *
 * Logs alerts in JSON format.
 *
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "conf.h"

#include "threads.h"
#include "tm-threads.h"
#include "threadvars.h"
#include "util-debug.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"

#include "detect.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-reference.h"
#include "app-layer-parser.h"
#include "app-layer-htp.h"
#include "app-layer-htp-xff.h"
#include "util-classification-config.h"
#include "util-syslog.h"

#include "output.h"
#include "output-json.h"
#include "output-json-http.h"
#include "output-json-tls.h"
#include "output-json-ssh.h"

#include "util-byte.h"
#include "util-privs.h"
#include "util-print.h"
#include "util-proto-name.h"
#include "util-optimize.h"
#include "util-buffer.h"
#include "util-logopenfile.h"
#include "util-crypt.h"

#define MODULE_NAME "JsonAlertLog"

#ifdef HAVE_LIBJANSSON

#define LOG_JSON_PAYLOAD 1
#define LOG_JSON_PACKET 2
#define LOG_JSON_PAYLOAD_BASE64 4
#define LOG_JSON_HTTP 8
#define LOG_JSON_TLS 16
#define LOG_JSON_SSH 32

#define JSON_STREAM_BUFFER_SIZE 4096

typedef struct AlertJsonOutputCtx_ {
    LogFileCtx* file_ctx;
    uint8_t flags;
    HttpXFFCfg *xff_cfg;
} AlertJsonOutputCtx;

typedef struct JsonAlertLogThread_ {
    /** LogFileCtx has the pointer to the file and a mutex to allow multithreading */
    LogFileCtx* file_ctx;
    MemBuffer *json_buffer;
    MemBuffer *payload_buffer;
    AlertJsonOutputCtx* json_output_ctx;
} JsonAlertLogThread;

/* Callback function to pack payload contents from a stream into a buffer
 * so we can report them in JSON output. */
static int AlertJsonDumpStreamSegmentCallback(const Packet *p, void *data, uint8_t *buf, uint32_t buflen)
{
    MemBuffer *payload = (MemBuffer *)data;
    MemBufferWriteRaw(payload, buf, buflen);

    return 1;
}

/** Handle the case where no JSON support is compiled in.
 *
 */
static void AlertJsonHttp(const Flow *f, json_t *js)
{
    HtpState *htp_state = (HtpState *)FlowGetAppState(f);
    if (htp_state) {
        uint64_t tx_id = AppLayerParserGetTransactionLogId(f->alparser);
        htp_tx_t *tx = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, htp_state, tx_id);

        if (tx) {
            json_t *hjs = json_object();
            if (unlikely(hjs == NULL))
                return;

            JsonHttpLogJSONBasic(hjs, tx);
            JsonHttpLogJSONExtended(hjs, tx);

            json_object_set_new(js, "http", hjs);
        }
    }

    return;
}

static void AlertJsonTls(const Flow *f, json_t *js)
{
    SSLState *ssl_state = (SSLState *)FlowGetAppState(f);
    if (ssl_state) {
        json_t *tjs = json_object();
        if (unlikely(tjs == NULL))
            return;

        JsonTlsLogJSONBasic(tjs, ssl_state);
        JsonTlsLogJSONExtended(tjs, ssl_state);

        json_object_set_new(js, "tls", tjs);
    }

    return;
}

static void AlertJsonSsh(const Flow *f, json_t *js)
{
    SshState *ssh_state = (SshState *)FlowGetAppState(f);
    if (ssh_state) {
        json_t *tjs = json_object();
        if (unlikely(tjs == NULL))
            return;

        JsonSshLogJSON(tjs, ssh_state);

        json_object_set_new(js, "ssh", tjs);
    }

    return;
}

void AlertJsonHeader(const PacketAlert *pa, json_t *js)
{
    char *action = "allowed";
    if (pa->action & (ACTION_REJECT|ACTION_REJECT_DST|ACTION_REJECT_BOTH)) {
        action = "blocked";
    } else if ((pa->action & ACTION_DROP) && EngineModeIsIPS()) {
        action = "blocked";
    }

    json_t *ajs = json_object();
    if (ajs == NULL) {
        json_decref(js);
        return;
    }

    json_object_set_new(ajs, "action", json_string(action));
    json_object_set_new(ajs, "gid", json_integer(pa->s->gid));
    json_object_set_new(ajs, "signature_id", json_integer(pa->s->id));
    json_object_set_new(ajs, "rev", json_integer(pa->s->rev));
    json_object_set_new(ajs, "signature",
            json_string((pa->s->msg) ? pa->s->msg : ""));
    json_object_set_new(ajs, "category",
            json_string((pa->s->class_msg) ? pa->s->class_msg : ""));
    json_object_set_new(ajs, "severity", json_integer(pa->s->prio));

    if (pa->flags & PACKET_ALERT_FLAG_TX)
        json_object_set_new(ajs, "tx_id", json_integer(pa->tx_id));

    /* alert */
    json_object_set_new(js, "alert", ajs);
}

static int AlertJson(ThreadVars *tv, JsonAlertLogThread *aft, const Packet *p)
{
    MemBuffer *payload = aft->payload_buffer;
    AlertJsonOutputCtx *json_output_ctx = aft->json_output_ctx;

    int i;

    if (p->alerts.cnt == 0)
        return TM_ECODE_OK;

    json_t *js = CreateJSONHeader((Packet *)p, 0, "alert");
    if (unlikely(js == NULL))
        return TM_ECODE_OK;

    for (i = 0; i < p->alerts.cnt; i++) {
        const PacketAlert *pa = &p->alerts.alerts[i];
        if (unlikely(pa->s == NULL)) {
            continue;
        }

        MemBufferReset(aft->json_buffer);

        /* alert */
        AlertJsonHeader(pa, js);

        if (json_output_ctx->flags & LOG_JSON_HTTP) {
            if (p->flow != NULL) {
                FLOWLOCK_RDLOCK(p->flow);
                uint16_t proto = FlowGetAppProtocol(p->flow);

                /* http alert */
                if (proto == ALPROTO_HTTP)
                    AlertJsonHttp(p->flow, js);

                FLOWLOCK_UNLOCK(p->flow);
            }
        }

        if (json_output_ctx->flags & LOG_JSON_TLS) {
            if (p->flow != NULL) {
                FLOWLOCK_RDLOCK(p->flow);
                uint16_t proto = FlowGetAppProtocol(p->flow);

                /* http alert */
                if (proto == ALPROTO_TLS)
                    AlertJsonTls(p->flow, js);

                FLOWLOCK_UNLOCK(p->flow);
            }
        }

        if (json_output_ctx->flags & LOG_JSON_SSH) {
            if (p->flow != NULL) {
                FLOWLOCK_RDLOCK(p->flow);
                uint16_t proto = FlowGetAppProtocol(p->flow);

                /* http alert */
                if (proto == ALPROTO_SSH)
                    AlertJsonSsh(p->flow, js);

                FLOWLOCK_UNLOCK(p->flow);
            }
        }

        /* payload */
        if (json_output_ctx->flags & (LOG_JSON_PAYLOAD | LOG_JSON_PAYLOAD_BASE64)) {
            int stream = (p->proto == IPPROTO_TCP) ?
                         (pa->flags & (PACKET_ALERT_FLAG_STATE_MATCH | PACKET_ALERT_FLAG_STREAM_MATCH) ?
                         1 : 0) : 0;

            /* Is this a stream?  If so, pack part of it into the payload field */
            if (stream) {
                uint8_t flag;

                MemBufferReset(payload);

                if (!EngineModeIsIPS()) {
                    if (p->flowflags & FLOW_PKT_TOSERVER) {
                        flag = FLOW_PKT_TOCLIENT;
                    } else {
                        flag = FLOW_PKT_TOSERVER;
                    }
                } else {
                    if (p->flowflags & FLOW_PKT_TOSERVER) {
                        flag = FLOW_PKT_TOSERVER;
                    } else {
                        flag = FLOW_PKT_TOCLIENT;
                    }
                }

                StreamSegmentForEach((const Packet *)p, flag,
                                    AlertJsonDumpStreamSegmentCallback,
                                    (void *)payload);

                if (json_output_ctx->flags & LOG_JSON_PAYLOAD_BASE64) {
                    unsigned long len = JSON_STREAM_BUFFER_SIZE * 2;
                    uint8_t encoded[len];
                    Base64Encode((unsigned char *)payload, payload->offset, encoded, &len);
                    json_object_set_new(js, "payload", json_string((char *)encoded));
                }

                if (json_output_ctx->flags & LOG_JSON_PAYLOAD) {
                    uint8_t printable_buf[payload->offset + 1];
                    uint32_t offset = 0;
                    PrintStringsToBuffer(printable_buf, &offset,
                                     sizeof(printable_buf),
                                     (unsigned char *)payload, payload->offset);
                    json_object_set_new(js, "payload_printable",
                                        json_string((char *)printable_buf));
                }
            } else {
                /* This is a single packet and not a stream */
                if (json_output_ctx->flags & LOG_JSON_PAYLOAD_BASE64) {
                    unsigned long len = p->payload_len * 2 + 1;
                    uint8_t encoded[len];
                    Base64Encode(p->payload, p->payload_len, encoded, &len);
                    json_object_set_new(js, "payload", json_string((char *)encoded));
                }

                if (json_output_ctx->flags & LOG_JSON_PAYLOAD) {
                    uint8_t printable_buf[p->payload_len + 1];
                    uint32_t offset = 0;
                    PrintStringsToBuffer(printable_buf, &offset,
                                     p->payload_len + 1,
                                     p->payload, p->payload_len);
                    json_object_set_new(js, "payload_printable", json_string((char *)printable_buf));
                }
            }

            json_object_set_new(js, "stream", json_integer(stream));
        }

        /* base64-encoded full packet */
        if (json_output_ctx->flags & LOG_JSON_PACKET) {
            unsigned long len = GET_PKT_LEN(p) * 2;
            uint8_t encoded_packet[len];
            Base64Encode((unsigned char*) GET_PKT_DATA(p), GET_PKT_LEN(p), encoded_packet, &len);
            json_object_set_new(js, "packet", json_string((char *)encoded_packet));
        }

        HttpXFFCfg *xff_cfg = json_output_ctx->xff_cfg;

        /* xff header */
        if (!(xff_cfg->flags & XFF_DISABLED) && p->flow != NULL) {
            int have_xff_ip = 0;
            char buffer[XFF_MAXLEN];

            FLOWLOCK_RDLOCK(p->flow);
            if (FlowGetAppProtocol(p->flow) == ALPROTO_HTTP) {
                if (pa->flags & PACKET_ALERT_FLAG_TX) {
                    have_xff_ip = HttpXFFGetIPFromTx(p, pa->tx_id, xff_cfg, buffer, XFF_MAXLEN);
                } else {
                    have_xff_ip = HttpXFFGetIP(p, xff_cfg, buffer, XFF_MAXLEN);
                }
            }
            FLOWLOCK_UNLOCK(p->flow);

            if (have_xff_ip) {
                if (xff_cfg->flags & XFF_EXTRADATA) {
                    json_object_set_new(js, "xff", json_string(buffer));
                }
                else if (xff_cfg->flags & XFF_OVERWRITE) {
                    if (p->flowflags & FLOW_PKT_TOCLIENT) {
                        json_object_set(js, "dest_ip", json_string(buffer));
                    } else {
                        json_object_set(js, "src_ip", json_string(buffer));
                    }
                }
            }
        }

        OutputJSONBuffer(js, aft->file_ctx, aft->json_buffer);
        json_object_del(js, "alert");
    }
    json_object_clear(js);
    json_decref(js);

    return TM_ECODE_OK;
}

static int AlertJsonDecoderEvent(ThreadVars *tv, JsonAlertLogThread *aft, const Packet *p)
{
    MemBuffer *buffer = (MemBuffer *)aft->json_buffer;
    int i;
    char timebuf[64];
    json_t *js;

    if (p->alerts.cnt == 0)
        return TM_ECODE_OK;

    CreateIsoTimeString(&p->ts, timebuf, sizeof(timebuf));

    for (i = 0; i < p->alerts.cnt; i++) {
        MemBufferReset(buffer);

        const PacketAlert *pa = &p->alerts.alerts[i];
        if (unlikely(pa->s == NULL)) {
            continue;
        }

        char *action = "allowed";
        if (pa->action & (ACTION_REJECT|ACTION_REJECT_DST|ACTION_REJECT_BOTH)) {
            action = "blocked";
        } else if ((pa->action & ACTION_DROP) && EngineModeIsIPS()) {
            action = "blocked";
        }

        char buf[(32 * 3) + 1];
        PrintRawLineHexBuf(buf, sizeof(buf), GET_PKT_DATA(p), GET_PKT_LEN(p) < 32 ? GET_PKT_LEN(p) : 32);

        js = json_object();
        if (js == NULL)
            return TM_ECODE_OK;

        json_t *ajs = json_object();
        if (ajs == NULL) {
            json_decref(js);
            return TM_ECODE_OK;
        }

        /* time & tx */
        json_object_set_new(js, "timestamp", json_string(timebuf));

        /* tuple */
        //json_object_set_new(js, "srcip", json_string(srcip));
        //json_object_set_new(js, "sp", json_integer(p->sp));
        //json_object_set_new(js, "dstip", json_string(dstip));
        //json_object_set_new(js, "dp", json_integer(p->dp));
        //json_object_set_new(js, "proto", json_integer(proto));

        json_object_set_new(ajs, "action", json_string(action));
        json_object_set_new(ajs, "gid", json_integer(pa->s->gid));
        json_object_set_new(ajs, "signature_id", json_integer(pa->s->id));
        json_object_set_new(ajs, "rev", json_integer(pa->s->rev));
        json_object_set_new(ajs, "signature",
                            json_string((pa->s->msg) ? pa->s->msg : ""));
        json_object_set_new(ajs, "category",
                            json_string((pa->s->class_msg) ? pa->s->class_msg : ""));
        json_object_set_new(ajs, "severity", json_integer(pa->s->prio));

        /* alert */
        json_object_set_new(js, "alert", ajs);
        OutputJSONBuffer(js, aft->file_ctx, buffer);
        json_object_clear(js);
        json_decref(js);
    }

    return TM_ECODE_OK;
}

static int JsonAlertLogger(ThreadVars *tv, void *thread_data, const Packet *p)
{
    JsonAlertLogThread *aft = thread_data;

    if (PKT_IS_IPV4(p) || PKT_IS_IPV6(p)) {
        return AlertJson(tv, aft, p);
    } else if (p->alerts.cnt > 0) {
        return AlertJsonDecoderEvent(tv, aft, p);
    }
    return 0;
}

static int JsonAlertLogCondition(ThreadVars *tv, const Packet *p)
{
    return (p->alerts.cnt ? TRUE : FALSE);
}

#define OUTPUT_BUFFER_SIZE 65535
static TmEcode JsonAlertLogThreadInit(ThreadVars *t, void *initdata, void **data)
{
    JsonAlertLogThread *aft = SCMalloc(sizeof(JsonAlertLogThread));
    if (unlikely(aft == NULL))
        return TM_ECODE_FAILED;
    memset(aft, 0, sizeof(JsonAlertLogThread));
    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for AlertFastLog.  \"initdata\" argument NULL");
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    aft->json_buffer = MemBufferCreateNew(OUTPUT_BUFFER_SIZE);
    if (aft->json_buffer == NULL) {
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    aft->payload_buffer = MemBufferCreateNew(JSON_STREAM_BUFFER_SIZE);
    if (aft->payload_buffer == NULL) {
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    /** Use the Output Context (file pointer and mutex) */
    AlertJsonOutputCtx *json_output_ctx = ((OutputCtx *)initdata)->data;
    aft->file_ctx = json_output_ctx->file_ctx;
    aft->json_output_ctx = json_output_ctx;

    *data = (void *)aft;
    return TM_ECODE_OK;
}

static TmEcode JsonAlertLogThreadDeinit(ThreadVars *t, void *data)
{
    JsonAlertLogThread *aft = (JsonAlertLogThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    MemBufferFree(aft->json_buffer);
    MemBufferFree(aft->payload_buffer);

    /* clear memory */
    memset(aft, 0, sizeof(JsonAlertLogThread));

    SCFree(aft);
    return TM_ECODE_OK;
}

static void JsonAlertLogDeInitCtx(OutputCtx *output_ctx)
{
    SCLogDebug("cleaning up output_ctx");
    LogFileCtx *logfile_ctx = (LogFileCtx *)output_ctx->data;
    LogFileFreeCtx(logfile_ctx);
    SCFree(output_ctx);
}

static void JsonAlertLogDeInitCtxSub(OutputCtx *output_ctx)
{
    SCLogDebug("cleaning up sub output_ctx %p", output_ctx);

    AlertJsonOutputCtx *json_output_ctx = (AlertJsonOutputCtx *) output_ctx->data;

    if (json_output_ctx != NULL) {
        HttpXFFCfg *xff_cfg = json_output_ctx->xff_cfg;
        if (xff_cfg != NULL) {
            SCFree(xff_cfg);
        }

        SCFree(json_output_ctx);
    }
    SCFree(output_ctx);
}

#define DEFAULT_LOG_FILENAME "alert.json"

/**
 * \brief Create a new LogFileCtx for "fast" output style.
 * \param conf The configuration node for this output.
 * \return A LogFileCtx pointer on success, NULL on failure.
 */
static OutputCtx *JsonAlertLogInitCtx(ConfNode *conf)
{
    LogFileCtx *logfile_ctx = LogFileNewCtx();
    if (logfile_ctx == NULL) {
        SCLogDebug("AlertFastLogInitCtx2: Could not create new LogFileCtx");
        return NULL;
    }

    if (SCConfLogOpenGeneric(conf, logfile_ctx, DEFAULT_LOG_FILENAME) < 0) {
        LogFileFreeCtx(logfile_ctx);
        return NULL;
    }

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL))
        return NULL;
    output_ctx->data = logfile_ctx;
    output_ctx->DeInit = JsonAlertLogDeInitCtx;

    return output_ctx;
}

/**
 * \brief Create a new LogFileCtx for "fast" output style.
 * \param conf The configuration node for this output.
 * \return A LogFileCtx pointer on success, NULL on failure.
 */
static OutputCtx *JsonAlertLogInitCtxSub(ConfNode *conf, OutputCtx *parent_ctx)
{
    AlertJsonThread *ajt = parent_ctx->data;
    AlertJsonOutputCtx *json_output_ctx = NULL;
    HttpXFFCfg *xff_cfg = NULL;

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL))
        return NULL;

    json_output_ctx = SCMalloc(sizeof(AlertJsonOutputCtx));
    if (unlikely(json_output_ctx == NULL)) {
        goto error;
    }
    memset(json_output_ctx, 0, sizeof(AlertJsonOutputCtx));

    xff_cfg = SCMalloc(sizeof(HttpXFFCfg));
    if (unlikely(xff_cfg == NULL)) {
        goto error;
    }
    memset(xff_cfg, 0, sizeof(HttpXFFCfg));

    json_output_ctx->file_ctx = ajt->file_ctx;
    json_output_ctx->xff_cfg = xff_cfg;

    if (conf != NULL) {
        const char *payload = ConfNodeLookupChildValue(conf, "payload");
        const char *packet  = ConfNodeLookupChildValue(conf, "packet");
        const char *payload_printable = ConfNodeLookupChildValue(conf, "payload-printable");
        const char *http = ConfNodeLookupChildValue(conf, "http");
        const char *tls = ConfNodeLookupChildValue(conf, "tls");
        const char *ssh = ConfNodeLookupChildValue(conf, "ssh");

        if (ssh != NULL) {
            if (ConfValIsTrue(ssh)) {
                json_output_ctx->flags |= LOG_JSON_SSH;
            }
        }
        if (tls != NULL) {
            if (ConfValIsTrue(tls)) {
                json_output_ctx->flags |= LOG_JSON_TLS;
            }
        }
        if (http != NULL) {
            if (ConfValIsTrue(http)) {
                json_output_ctx->flags |= LOG_JSON_HTTP;
            }
        }
        if (payload_printable != NULL) {
            if (ConfValIsTrue(payload_printable)) {
                json_output_ctx->flags |= LOG_JSON_PAYLOAD;
            }
        }
        if (payload != NULL) {
            if (ConfValIsTrue(payload)) {
                json_output_ctx->flags |= LOG_JSON_PAYLOAD_BASE64;
            }
        }
        if (packet != NULL) {
            if (ConfValIsTrue(packet)) {
                json_output_ctx->flags |= LOG_JSON_PACKET;
            }
        }

        HttpXFFGetCfg(conf, xff_cfg);
    }

    output_ctx->data = json_output_ctx;
    output_ctx->DeInit = JsonAlertLogDeInitCtxSub;

    return output_ctx;

error:
    if (json_output_ctx != NULL) {
        SCFree(json_output_ctx);
    }
    if (output_ctx != NULL) {
        SCFree(output_ctx);
    }

    return NULL;
}

void TmModuleJsonAlertLogRegister (void)
{
    tmm_modules[TMM_JSONALERTLOG].name = MODULE_NAME;
    tmm_modules[TMM_JSONALERTLOG].ThreadInit = JsonAlertLogThreadInit;
    tmm_modules[TMM_JSONALERTLOG].ThreadDeinit = JsonAlertLogThreadDeinit;
    tmm_modules[TMM_JSONALERTLOG].cap_flags = 0;
    tmm_modules[TMM_JSONALERTLOG].flags = TM_FLAG_LOGAPI_TM;

    OutputRegisterPacketModule(MODULE_NAME, "alert-json-log",
            JsonAlertLogInitCtx, JsonAlertLogger, JsonAlertLogCondition);
    OutputRegisterPacketSubModule("eve-log", MODULE_NAME, "eve-log.alert",
            JsonAlertLogInitCtxSub, JsonAlertLogger, JsonAlertLogCondition);
}

#else

static TmEcode OutputJsonThreadInit(ThreadVars *t, void *initdata, void **data)
{
    SCLogInfo("Can't init JSON output - JSON support was disabled during build.");
    return TM_ECODE_FAILED;
}

void TmModuleJsonAlertLogRegister (void)
{
    tmm_modules[TMM_JSONALERTLOG].name = MODULE_NAME;
    tmm_modules[TMM_JSONALERTLOG].ThreadInit = OutputJsonThreadInit;
}

#endif

