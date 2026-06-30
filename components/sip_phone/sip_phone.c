#include "sip_phone.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "audio_hw.h"
#include "call_recorder.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/md5.h"
#include "sdkconfig.h"

static const char *TAG = "sip_phone";

static sip_phone_state_t s_state = SIP_PHONE_STATE_OFFLINE;
static uint32_t s_cseq = 1;
static uint32_t s_dialog_cseq = 1;
static TaskHandle_t s_register_refresh_task = NULL;
static TaskHandle_t s_incoming_task = NULL;
static TaskHandle_t s_rtp_rx_task = NULL;
static TaskHandle_t s_rtp_tx_task = NULL;
static TaskHandle_t s_rtp_audio_task = NULL;
static TaskHandle_t s_ring_signal_task = NULL;
static TaskHandle_t s_ringback_task = NULL;
static QueueHandle_t s_rtp_audio_queue;
static QueueHandle_t s_sip_response_queue;
static int s_incoming_sock = -1;
static SemaphoreHandle_t s_sip_io_mutex;
static SemaphoreHandle_t s_sip_transaction_mutex;
static char s_register_request[1280];
static char s_register_response[1536];
static char s_outgoing_response[1536];
static volatile bool s_rtp_rx_running;
static volatile bool s_rtp_tx_running;
static volatile bool s_ring_signal_running;
static volatile bool s_ringback_running;
static volatile bool s_outgoing_cancel_requested;
static volatile bool s_outgoing_invite_active;
static struct sockaddr_storage s_outgoing_dest_addr;
static socklen_t s_outgoing_dest_addr_len;
static char s_outgoing_local_ip[16];
static char s_outgoing_call_id[48];
static char s_outgoing_branch[40];
static char s_outgoing_from[256];
static char s_outgoing_to[256];
static uint32_t s_outgoing_invite_cseq;

#define SIP_RX_TIMEOUT_MS 7000
#define SIP_REGISTER_EXPIRES_SECONDS 120
#define SIP_REGISTER_REFRESH_SECONDS (SIP_REGISTER_EXPIRES_SECONDS - 30)
#define SIP_REGISTER_RETRY_SECONDS 10
#define SIP_REGISTER_START_ATTEMPTS 4
#define SIP_REGISTER_START_RETRY_SECONDS 3
#define SIP_REGISTER_REFRESH_FAST_ATTEMPTS 6
#define SIP_INCOMING_SOCKET_WAIT_MS 3000
#define SIP_RESPONSE_QUEUE_LENGTH 4
#define SIP_INVITE_RESPONSE_TIMEOUT_MS 30000
#define SIP_RING_SIGNAL_STACK_WORDS 4096
#define SIP_RINGBACK_STACK_WORDS 4096
#define SIP_REGISTER_REFRESH_STACK_WORDS 8192
#define SIP_INCOMING_STACK_WORDS 8192
#define SIP_INCOMING_RECV_TIMEOUT_MS 50
#define SIP_LOCAL_RTP_PORT 4000
#define SIP_AUDIO_CODEC_PAYLOAD 8
#define SIP_AUDIO_CODEC_NAME "PCMA"
#define SIP_AUDIO_CODEC_RATE 8000
#define RTP_RX_STACK_WORDS 6144
#define RTP_TX_STACK_WORDS 6144
#define RTP_RX_TIMEOUT_MS 1000
#define RTP_PACKET_BUFFER_BYTES 256
#define RTP_TX_PAYLOAD_BYTES 160
#define RTP_HEADER_BYTES 12
#define RTP_MAX_AUDIO_SAMPLES 320
#define RTP_AUDIO_QUEUE_LENGTH 10
#define RTP_AUDIO_FRAME_MS 20
#define RTP_AUDIO_TASK_PRIORITY 8
#define RTP_RX_TASK_PRIORITY 7
#define RTP_TX_TASK_PRIORITY 6
#define RTP_TX_SEND_BACKOFF_MS 5
#define RTP_MIC_ATTENUATION_SHIFT 2
#define RING_SIGNAL_FRAME_MS 20
#define RING_SIGNAL_BEEP_MS 450
#define RING_SIGNAL_PAUSE_MS 1200
#define RING_SIGNAL_AMPLITUDE 6500
#define RINGBACK_TONE_HZ 425
#define RINGBACK_TONE_MS 1000
#define RINGBACK_PAUSE_MS 4000
#define RINGBACK_AMPLITUDE 5500
#define SIP_UDP_TRANSPORT "UDP"

typedef struct {
    char realm[96];
    char nonce[160];
    char opaque[96];
    char qop[48];
    bool has_qop_auth;
} sip_digest_challenge_t;

typedef struct {
    bool active;
    struct sockaddr_storage remote_addr;
    socklen_t remote_addr_len;
    char via[256];
    char from[256];
    char to[256];
    char call_id[128];
    char cseq[96];
    char local_tag[16];
    char remote_sdp[512];
} sip_incoming_call_t;

typedef struct {
    size_t sample_count;
    int16_t samples[RTP_MAX_AUDIO_SAMPLES];
} rtp_audio_frame_t;

typedef struct {
    size_t len;
    char data[1536];
} sip_response_msg_t;

static sip_incoming_call_t s_incoming_call;

static esp_err_t build_basic_sdp(char *out, size_t out_len);
static esp_err_t start_ringback(void);
static void stop_ringback(void);
static void start_call_recording_from_dialog(void);

esp_err_t sip_phone_init(void)
{
    s_sip_io_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_sip_io_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create SIP I/O mutex failed");
    s_sip_transaction_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_sip_transaction_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create SIP transaction mutex failed");

    s_sip_response_queue = xQueueCreate(SIP_RESPONSE_QUEUE_LENGTH, sizeof(sip_response_msg_t));
    ESP_RETURN_ON_FALSE(s_sip_response_queue != NULL, ESP_ERR_NO_MEM, TAG, "create SIP response queue failed");

    s_state = SIP_PHONE_STATE_OFFLINE;
    ESP_LOGI(TAG, "SIP phone core placeholder ready");
    return ESP_OK;
}

sip_phone_state_t sip_phone_get_state(void)
{
    return s_state;
}

static bool sip_phone_is_configured(void)
{
    return strlen(CONFIG_IPPHONE_SIP_SERVER) > 0 && strlen(CONFIG_IPPHONE_SIP_USER) > 0;
}

static esp_err_t resolve_sip_server(struct sockaddr_storage *dest_addr, socklen_t *dest_addr_len)
{
    char port[8] = {0};
    snprintf(port, sizeof(port), "%d", CONFIG_IPPHONE_SIP_PORT);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *result = NULL;

    int err = getaddrinfo(CONFIG_IPPHONE_SIP_SERVER, port, &hints, &result);
    ESP_RETURN_ON_FALSE(err == 0 && result != NULL, ESP_FAIL, TAG, "resolve SIP server failed: %d", err);

    memcpy(dest_addr, result->ai_addr, result->ai_addrlen);
    *dest_addr_len = (socklen_t)result->ai_addrlen;
    freeaddrinfo(result);
    return ESP_OK;
}

static esp_err_t set_socket_rx_timeout(int sock, int timeout_ms)
{
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    ESP_RETURN_ON_FALSE(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
                        ESP_FAIL,
                        TAG,
                        "set SIP socket timeout failed: errno=%d",
                        errno);
    return ESP_OK;
}

static esp_err_t bind_socket_to_sip_port(int sock)
{
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_IPPHONE_SIP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    ESP_RETURN_ON_FALSE(bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) == 0,
                        ESP_FAIL,
                        TAG,
                        "bind SIP socket to UDP port %d failed: errno=%d",
                        CONFIG_IPPHONE_SIP_PORT,
                        errno);
    return ESP_OK;
}

static int parse_sip_status_code(const char *response)
{
    int code = 0;
    if (sscanf(response, "SIP/2.0 %d", &code) == 1) {
        return code;
    }
    return 0;
}

static int ascii_tolower(int c)
{
    return tolower((unsigned char)c);
}

static const char *strcasestr_ascii(const char *haystack, const char *needle)
{
    if (needle[0] == '\0') {
        return haystack;
    }

    for (const char *p = haystack; *p != '\0'; ++p) {
        const char *h = p;
        const char *n = needle;
        while (*h != '\0' && *n != '\0' && ascii_tolower(*h) == ascii_tolower(*n)) {
            ++h;
            ++n;
        }
        if (*n == '\0') {
            return p;
        }
    }
    return NULL;
}

static const char *find_sip_header(const char *response, const char *header_name)
{
    const size_t header_len = strlen(header_name);
    const char *line = response;

    while (*line != '\0') {
        const char *line_end = strstr(line, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);

        if (line_len == 0) {
            return NULL;
        }

        if (line_len > header_len && line[header_len] == ':') {
            bool match = true;
            for (size_t i = 0; i < header_len; ++i) {
                if (ascii_tolower(line[i]) != ascii_tolower(header_name[i])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                const char *value = line + header_len + 1;
                while (*value == ' ' || *value == '\t') {
                    ++value;
                }
                return value;
            }
        }

        if (line_end == NULL) {
            break;
        }
        line = line_end + 2;
    }

    return NULL;
}

static esp_err_t copy_header_line_value(const char *value, char *out, size_t out_len)
{
    ESP_RETURN_ON_FALSE(value != NULL && out != NULL && out_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid header copy args");

    const char *end = strstr(value, "\r\n");
    size_t len = end ? (size_t)(end - value) : strlen(value);
    ESP_RETURN_ON_FALSE(len < out_len, ESP_ERR_INVALID_SIZE, TAG, "SIP auth header is too large");

    memcpy(out, value, len);
    out[len] = '\0';
    return ESP_OK;
}

static esp_err_t copy_sip_header_value(const char *message, const char *header_name, char *out, size_t out_len)
{
    const char *value = find_sip_header(message, header_name);
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_NOT_FOUND, TAG, "SIP header %s not found", header_name);
    return copy_header_line_value(value, out, out_len);
}

static bool sip_response_matches_cseq_method(const char *response, const char *method)
{
    const char *cseq = find_sip_header(response, "CSeq");
    if (cseq == NULL) {
        return false;
    }

    const char *line_end = strstr(cseq, "\r\n");
    size_t line_len = line_end ? (size_t)(line_end - cseq) : strlen(cseq);
    char cseq_line[96] = {0};
    if (line_len >= sizeof(cseq_line)) {
        line_len = sizeof(cseq_line) - 1;
    }
    memcpy(cseq_line, cseq, line_len);
    cseq_line[line_len] = '\0';

    return strcasestr_ascii(cseq_line, method) != NULL;
}

static bool sip_response_matches_transaction(const char *response, const char *method, const char *call_id)
{
    if (!sip_response_matches_cseq_method(response, method)) {
        return false;
    }

    if (call_id == NULL || call_id[0] == '\0') {
        return true;
    }

    char response_call_id[128] = {0};
    if (copy_sip_header_value(response, "Call-ID", response_call_id, sizeof(response_call_id)) != ESP_OK) {
        return false;
    }

    return strcmp(response_call_id, call_id) == 0;
}

static bool sip_header_has_param(const char *value, const char *param)
{
    return strcasestr_ascii(value, param) != NULL;
}

static esp_err_t get_local_ip_string(char *out, size_t out_len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_ERR_INVALID_STATE, TAG, "Wi-Fi netif not found");

    esp_netif_ip_info_t ip_info = {0};
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(netif, &ip_info), TAG, "get local IP failed");

    int len = snprintf(out, out_len, IPSTR, IP2STR(&ip_info.ip));
    ESP_RETURN_ON_FALSE(len > 0 && len < (int)out_len, ESP_ERR_INVALID_SIZE, TAG, "local IP string too large");
    return ESP_OK;
}

static esp_err_t parse_digest_param(const char *challenge, const char *key, char *out, size_t out_len)
{
    ESP_RETURN_ON_FALSE(challenge != NULL && key != NULL && out != NULL && out_len > 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid digest param args");

    const size_t key_len = strlen(key);
    const char *p = challenge;

    while ((p = strcasestr_ascii(p, key)) != NULL) {
        const bool valid_prefix = (p == challenge) || p[-1] == ' ' || p[-1] == '\t' || p[-1] == ',';
        const char *after_key = p + key_len;
        while (*after_key == ' ' || *after_key == '\t') {
            ++after_key;
        }

        if (valid_prefix && *after_key == '=') {
            const char *value = after_key + 1;
            while (*value == ' ' || *value == '\t') {
                ++value;
            }

            const char *end = value;
            if (*value == '"') {
                ++value;
                end = strchr(value, '"');
                ESP_RETURN_ON_FALSE(end != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "unterminated digest %s", key);
            } else {
                while (*end != '\0' && *end != ',' && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') {
                    ++end;
                }
            }

            size_t len = (size_t)(end - value);
            ESP_RETURN_ON_FALSE(len < out_len, ESP_ERR_INVALID_SIZE, TAG, "digest %s is too large", key);
            memcpy(out, value, len);
            out[len] = '\0';
            return ESP_OK;
        }

        p += key_len;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t parse_digest_challenge(const char *response,
                                        int status_code,
                                        sip_digest_challenge_t *challenge)
{
    const char *header = NULL;

    if (status_code == 407) {
        header = find_sip_header(response, "Proxy-Authenticate");
    } else {
        header = find_sip_header(response, "WWW-Authenticate");
    }

    ESP_RETURN_ON_FALSE(header != NULL, ESP_ERR_NOT_FOUND, TAG, "SIP digest challenge header not found");

    char header_value[384] = {0};
    ESP_RETURN_ON_ERROR(copy_header_line_value(header, header_value, sizeof(header_value)), TAG, "copy auth header failed");
    ESP_RETURN_ON_FALSE(strcasestr_ascii(header_value, "Digest") != NULL,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "unsupported SIP auth challenge: %.80s",
                        header_value);

    memset(challenge, 0, sizeof(*challenge));
    ESP_RETURN_ON_ERROR(parse_digest_param(header_value, "realm", challenge->realm, sizeof(challenge->realm)),
                        TAG,
                        "digest realm missing");
    ESP_RETURN_ON_ERROR(parse_digest_param(header_value, "nonce", challenge->nonce, sizeof(challenge->nonce)),
                        TAG,
                        "digest nonce missing");

    if (parse_digest_param(header_value, "opaque", challenge->opaque, sizeof(challenge->opaque)) != ESP_OK) {
        challenge->opaque[0] = '\0';
    }

    if (parse_digest_param(header_value, "qop", challenge->qop, sizeof(challenge->qop)) == ESP_OK) {
        challenge->has_qop_auth = strcasestr_ascii(challenge->qop, "auth") != NULL;
        ESP_RETURN_ON_FALSE(challenge->has_qop_auth,
                            ESP_ERR_NOT_SUPPORTED,
                            TAG,
                            "SIP digest qop is not supported: %s",
                            challenge->qop);
    }

    return ESP_OK;
}

static void md5_hex(const char *input, char output[33])
{
    unsigned char digest[16] = {0};
    mbedtls_md5((const unsigned char *)input, strlen(input), digest);

    for (size_t i = 0; i < sizeof(digest); ++i) {
        snprintf(output + (i * 2), 3, "%02x", digest[i]);
    }
    output[32] = '\0';
}

static esp_err_t build_digest_authorization(const sip_digest_challenge_t *challenge,
                                            int status_code,
                                            const char *method,
                                            const char *uri,
                                            uint32_t nc,
                                            const char *cnonce,
                                            char *out,
                                            size_t out_len)
{
    const char *username = CONFIG_IPPHONE_SIP_USER;
    const char *password = CONFIG_IPPHONE_SIP_PASSWORD;
    const char *auth_header = (status_code == 407) ? "Proxy-Authorization" : "Authorization";

    ESP_RETURN_ON_FALSE(strlen(password) > 0, ESP_ERR_INVALID_STATE, TAG, "SIP password is empty; Digest auth cannot continue");
    ESP_RETURN_ON_FALSE(method != NULL && method[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "digest method is empty");
    ESP_RETURN_ON_FALSE(uri != NULL && uri[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "digest URI is empty");

    char ha1_input[256] = {0};
    char ha2_input[160] = {0};
    char response_input[512] = {0};
    char ha1[33] = {0};
    char ha2[33] = {0};
    char response[33] = {0};

    int len = snprintf(ha1_input, sizeof(ha1_input), "%s:%s:%s", username, challenge->realm, password);
    ESP_RETURN_ON_FALSE(len > 0 && len < (int)sizeof(ha1_input), ESP_ERR_INVALID_SIZE, TAG, "HA1 input too large");
    md5_hex(ha1_input, ha1);

    len = snprintf(ha2_input, sizeof(ha2_input), "%s:%s", method, uri);
    ESP_RETURN_ON_FALSE(len > 0 && len < (int)sizeof(ha2_input), ESP_ERR_INVALID_SIZE, TAG, "HA2 input too large");
    md5_hex(ha2_input, ha2);

    if (challenge->has_qop_auth) {
        len = snprintf(response_input,
                       sizeof(response_input),
                       "%s:%s:%08lx:%s:auth:%s",
                       ha1,
                       challenge->nonce,
                       (unsigned long)nc,
                       cnonce,
                       ha2);
    } else {
        len = snprintf(response_input, sizeof(response_input), "%s:%s:%s", ha1, challenge->nonce, ha2);
    }
    ESP_RETURN_ON_FALSE(len > 0 && len < (int)sizeof(response_input),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "digest response input too large");
    md5_hex(response_input, response);

    if (challenge->has_qop_auth) {
        len = snprintf(out,
                       out_len,
                       "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                       "algorithm=MD5, response=\"%s\", qop=auth, nc=%08lx, cnonce=\"%s\"",
                       auth_header,
                       username,
                       challenge->realm,
                       challenge->nonce,
                       uri,
                       response,
                       (unsigned long)nc,
                       cnonce);
    } else {
        len = snprintf(out,
                       out_len,
                       "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                       "algorithm=MD5, response=\"%s\"",
                       auth_header,
                       username,
                       challenge->realm,
                       challenge->nonce,
                       uri,
                       response);
    }
    ESP_RETURN_ON_FALSE(len > 0 && len < (int)out_len, ESP_ERR_INVALID_SIZE, TAG, "Authorization header too large");

    if (challenge->opaque[0] != '\0') {
        size_t used = strlen(out);
        len = snprintf(out + used, out_len - used, ", opaque=\"%s\"", challenge->opaque);
        ESP_RETURN_ON_FALSE(len > 0 && len < (int)(out_len - used),
                            ESP_ERR_INVALID_SIZE,
                            TAG,
                            "Authorization opaque too large");
    }

    return ESP_OK;
}

static esp_err_t send_register_request(int sock,
                                       const struct sockaddr_storage *dest_addr,
                                       socklen_t dest_addr_len,
                                       const char *call_id,
                                       const char *from_tag,
                                       uint32_t cseq,
                                       const char *authorization,
                                       char *response,
                                       size_t response_len,
                                       int *status_code,
                                       bool use_response_queue)
{
    char branch[40] = {0};
    char local_ip[16] = {0};
    snprintf(branch, sizeof(branch), "z9hG4bK-%08lx", (unsigned long)esp_random());
    esp_err_t err = get_local_ip_string(local_ip, sizeof(local_ip));
    if (err != ESP_OK) {
        strlcpy(local_ip, CONFIG_IPPHONE_WIFI_HOSTNAME, sizeof(local_ip));
    }

    char auth_line[480] = {0};
    if (authorization != NULL && authorization[0] != '\0') {
        int auth_len = snprintf(auth_line, sizeof(auth_line), "%s\r\n", authorization);
        ESP_RETURN_ON_FALSE(auth_len > 0 && auth_len < (int)sizeof(auth_line),
                            ESP_ERR_INVALID_SIZE,
                            TAG,
                            "SIP Authorization line too large");
    }

    memset(s_register_request, 0, sizeof(s_register_request));
    int request_len = snprintf(s_register_request,
                               sizeof(s_register_request),
                               "REGISTER sip:%s SIP/2.0\r\n"
                               "Via: SIP/2.0/%s %s;branch=%s;rport\r\n"
                               "Max-Forwards: 70\r\n"
                               "From: <sip:%s@%s>;tag=%s\r\n"
                               "To: <sip:%s@%s>\r\n"
                               "Call-ID: %s\r\n"
                               "CSeq: %lu REGISTER\r\n"
                               "Contact: <sip:%s@%s:%d;transport=udp>\r\n"
                               "Expires: %d\r\n"
                               "User-Agent: esp32-ipphone\r\n"
                               "%s"
                               "Content-Length: 0\r\n"
                               "\r\n",
                               CONFIG_IPPHONE_SIP_SERVER,
                               SIP_UDP_TRANSPORT,
                               CONFIG_IPPHONE_WIFI_HOSTNAME,
                               branch,
                               CONFIG_IPPHONE_SIP_USER,
                               CONFIG_IPPHONE_SIP_SERVER,
                               from_tag,
                               CONFIG_IPPHONE_SIP_USER,
                               CONFIG_IPPHONE_SIP_SERVER,
                               call_id,
                               (unsigned long)cseq,
                               CONFIG_IPPHONE_SIP_USER,
                               local_ip,
                               CONFIG_IPPHONE_SIP_PORT,
                               SIP_REGISTER_EXPIRES_SECONDS,
                               auth_line);
    ESP_RETURN_ON_FALSE(request_len > 0 && request_len < (int)sizeof(s_register_request),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "SIP REGISTER request too large");

    int sent = sendto(sock, s_register_request, request_len, 0, (const struct sockaddr *)dest_addr, dest_addr_len);
    if (sent < 0) {
        ESP_LOGE(TAG, "send SIP REGISTER failed: errno=%d", errno);
        return ESP_FAIL;
    }

    *status_code = 0;
    int response_attempts = 0;
    int ignored_responses = 0;
    while (response_attempts < 4) {
        int received = -1;
        if (use_response_queue) {
            sip_response_msg_t queued_response = {0};
            if (xQueueReceive(s_sip_response_queue, &queued_response, pdMS_TO_TICKS(SIP_RX_TIMEOUT_MS)) != pdTRUE) {
                ESP_LOGE(TAG, "SIP REGISTER response timeout waiting on listener queue");
                return ESP_ERR_TIMEOUT;
            }

            received = (int)queued_response.len;
            if ((size_t)received >= response_len) {
                received = (int)response_len - 1;
            }
            memcpy(response, queued_response.data, received);
        } else {
            received = recvfrom(sock, response, response_len - 1, 0, NULL, NULL);
            if (received < 0) {
                ESP_LOGE(TAG, "SIP REGISTER response timeout/error: errno=%d", errno);
                return ESP_ERR_TIMEOUT;
            }
        }

        response[received] = '\0';
        if (use_response_queue && !sip_response_matches_transaction(response, "REGISTER", call_id)) {
            if (++ignored_responses >= SIP_RESPONSE_QUEUE_LENGTH) {
                ESP_LOGE(TAG, "too many queued SIP responses ignored while waiting for REGISTER");
                return ESP_ERR_TIMEOUT;
            }
            ESP_LOGI(TAG, "queued SIP response ignored while waiting for REGISTER");
            continue;
        }

        ++response_attempts;
        *status_code = parse_sip_status_code(response);
        ESP_LOGI(TAG, "SIP REGISTER response: %d", *status_code);

        if (*status_code >= 200) {
            return ESP_OK;
        }

        ESP_LOGI(TAG, "SIP provisional response ignored");
    }

    return ESP_OK;
}

static esp_err_t send_register_on_socket(int sock,
                                         const struct sockaddr_storage *dest_addr,
                                         socklen_t dest_addr_len,
                                         bool use_response_queue)
{
    char call_id[48] = {0};
    char from_tag[16] = {0};
    snprintf(call_id, sizeof(call_id), "%08lx@esp32-ipphone", (unsigned long)esp_random());
    snprintf(from_tag, sizeof(from_tag), "%08lx", (unsigned long)esp_random());

    ESP_LOGI(TAG,
             "sending SIP REGISTER to %s:%d as user %s",
             CONFIG_IPPHONE_SIP_SERVER,
             CONFIG_IPPHONE_SIP_PORT,
             CONFIG_IPPHONE_SIP_USER);

    int status_code = 0;
    memset(s_register_response, 0, sizeof(s_register_response));
    esp_err_t err = send_register_request(sock,
                                          dest_addr,
                                          dest_addr_len,
                                          call_id,
                                          from_tag,
                                          s_cseq++,
                                          NULL,
                                          s_register_response,
                                          sizeof(s_register_response),
                                          &status_code,
                                          use_response_queue);
    if (err != ESP_OK) {
        return err;
    }

    if (status_code == 401 || status_code == 407) {
        sip_digest_challenge_t challenge = {0};
        err = parse_digest_challenge(s_register_response, status_code, &challenge);
        if (err != ESP_OK) {
            return err;
        }

        char cnonce[17] = {0};
        snprintf(cnonce, sizeof(cnonce), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());

        char authorization[560] = {0};
        char register_uri[96] = {0};
        snprintf(register_uri, sizeof(register_uri), "sip:%s", CONFIG_IPPHONE_SIP_SERVER);
        err = build_digest_authorization(&challenge,
                                         status_code,
                                         "REGISTER",
                                         register_uri,
                                         1,
                                         cnonce,
                                         authorization,
                                         sizeof(authorization));
        if (err != ESP_OK) {
            return err;
        }

        ESP_LOGI(TAG, "SIP digest challenge received, sending authenticated REGISTER");
        err = send_register_request(sock,
                                    dest_addr,
                                    dest_addr_len,
                                    call_id,
                                    from_tag,
                                    s_cseq++,
                                    authorization,
                                    s_register_response,
                                    sizeof(s_register_response),
                                    &status_code,
                                    use_response_queue);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (status_code == 200) {
        ESP_LOGI(TAG, "SIP registration accepted");
        return ESP_OK;
    }

    if (status_code == 401 || status_code == 407) {
        ESP_LOGW(TAG, "SIP authentication rejected by server");
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGW(TAG, "unexpected SIP final response: %.120s", s_register_response);
    return ESP_FAIL;
}

static esp_err_t send_register(void)
{
    struct sockaddr_storage dest_addr = {0};
    socklen_t dest_addr_len = 0;
    ESP_RETURN_ON_ERROR(resolve_sip_server(&dest_addr, &dest_addr_len), TAG, "SIP server resolve failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_sip_transaction_mutex, pdMS_TO_TICKS(SIP_RX_TIMEOUT_MS * 3)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "take SIP transaction mutex for REGISTER failed");

    esp_err_t err = ESP_OK;
    if (s_incoming_sock >= 0) {
        xQueueReset(s_sip_response_queue);
        err = send_register_on_socket(s_incoming_sock, &dest_addr, dest_addr_len, true);
        xSemaphoreGive(s_sip_transaction_mutex);
        return err;
    }

    int sock = socket(dest_addr.ss_family, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        xSemaphoreGive(s_sip_transaction_mutex);
        ESP_LOGE(TAG, "create SIP UDP socket failed: errno=%d", errno);
        return ESP_FAIL;
    }

    err = bind_socket_to_sip_port(sock);
    if (err == ESP_OK) {
        err = set_socket_rx_timeout(sock, SIP_RX_TIMEOUT_MS);
    }
    if (err == ESP_OK) {
        err = send_register_on_socket(sock, &dest_addr, dest_addr_len, false);
    }

    close(sock);
    xSemaphoreGive(s_sip_transaction_mutex);
    return err;
}

static bool sip_message_starts_with_method(const char *message, const char *method)
{
    size_t method_len = strlen(method);
    return strncmp(message, method, method_len) == 0 && message[method_len] == ' ';
}

static const char *sip_message_body(const char *message)
{
    const char *body = strstr(message, "\r\n\r\n");
    return body == NULL ? "" : body + 4;
}

static bool sdp_supports_g711a(const char *sdp)
{
    if (strcasestr_ascii(sdp, "a=rtpmap:8 PCMA/8000") != NULL) {
        return true;
    }

    const char *media = strcasestr_ascii(sdp, "m=audio ");
    if (media == NULL) {
        return false;
    }

    const char *line_end = strstr(media, "\r\n");
    if (line_end == NULL) {
        line_end = media + strlen(media);
    }

    const char *payload = media;
    while (payload < line_end) {
        while (payload < line_end && !isdigit((unsigned char)*payload)) {
            ++payload;
        }

        int value = 0;
        int digits = 0;
        while (payload < line_end && isdigit((unsigned char)*payload)) {
            value = (value * 10) + (*payload - '0');
            ++payload;
            ++digits;
        }

        if (digits > 0 && value == SIP_AUDIO_CODEC_PAYLOAD) {
            return true;
        }
    }

    return false;
}

static esp_err_t parse_remote_rtp_endpoint(const char *sdp,
                                           struct sockaddr_storage *remote_addr,
                                           socklen_t *remote_addr_len,
                                           uint16_t *remote_port)
{
    ESP_RETURN_ON_FALSE(sdp != NULL && remote_addr != NULL && remote_addr_len != NULL && remote_port != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid RTP endpoint args");

    const char *media = strcasestr_ascii(sdp, "m=audio ");
    ESP_RETURN_ON_FALSE(media != NULL, ESP_ERR_NOT_FOUND, TAG, "SDP m=audio not found");

    int port = 0;
    ESP_RETURN_ON_FALSE(sscanf(media, "m=audio %d", &port) == 1 && port > 0 && port <= 65535,
                        ESP_ERR_INVALID_RESPONSE,
                        TAG,
                        "SDP audio port invalid");

    const char *connection = strcasestr_ascii(sdp, "c=IN IP4 ");
    ESP_RETURN_ON_FALSE(connection != NULL, ESP_ERR_NOT_FOUND, TAG, "SDP c=IN IP4 not found");
    connection += strlen("c=IN IP4 ");

    char ip[16] = {0};
    size_t ip_len = 0;
    while (connection[ip_len] != '\0' &&
           connection[ip_len] != '\r' &&
           connection[ip_len] != '\n' &&
           connection[ip_len] != ' ' &&
           ip_len < sizeof(ip) - 1) {
        ip[ip_len] = connection[ip_len];
        ++ip_len;
    }
    ip[ip_len] = '\0';

    struct sockaddr_in *addr = (struct sockaddr_in *)remote_addr;
    memset(remote_addr, 0, sizeof(*remote_addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);
    ESP_RETURN_ON_FALSE(inet_pton(AF_INET, ip, &addr->sin_addr) == 1,
                        ESP_ERR_INVALID_RESPONSE,
                        TAG,
                        "SDP RTP IP invalid: %s",
                        ip);

    *remote_addr_len = sizeof(*addr);
    *remote_port = (uint16_t)port;
    return ESP_OK;
}

static esp_err_t format_to_with_tag(char *out, size_t out_len, const char *to, const char *tag)
{
    if (sip_header_has_param(to, ";tag=")) {
        strlcpy(out, to, out_len);
        ESP_RETURN_ON_FALSE(strlen(to) < out_len, ESP_ERR_INVALID_SIZE, TAG, "To header too large");
        return ESP_OK;
    }

    char original_to[256] = {0};
    strlcpy(original_to, to, sizeof(original_to));
    ESP_RETURN_ON_FALSE(strlen(to) < sizeof(original_to), ESP_ERR_INVALID_SIZE, TAG, "To header too large");

    int len = snprintf(out, out_len, "%s;tag=%s", original_to, tag);
    ESP_RETURN_ON_FALSE(len > 0 && len < (int)out_len, ESP_ERR_INVALID_SIZE, TAG, "tagged To header too large");
    return ESP_OK;
}

static esp_err_t copy_sip_uri_user(const char *header, char *out, size_t out_len)
{
    ESP_RETURN_ON_FALSE(header != NULL && out != NULL && out_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid SIP URI args");

    const char *sip = strcasestr_ascii(header, "sip:");
    ESP_RETURN_ON_FALSE(sip != NULL, ESP_ERR_NOT_FOUND, TAG, "SIP URI not found");
    sip += 4;

    const char *end = sip;
    while (*end != '\0' && *end != '@' && *end != '>' && *end != ';' && *end != ' ' && *end != '\t') {
        ++end;
    }

    size_t len = (size_t)(end - sip);
    ESP_RETURN_ON_FALSE(len > 0 && len < out_len, ESP_ERR_INVALID_SIZE, TAG, "SIP URI user too large");
    memcpy(out, sip, len);
    out[len] = '\0';
    return ESP_OK;
}

static esp_err_t copy_sip_contact_uri(const char *message, char *out, size_t out_len)
{
    char contact[256] = {0};
    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "Contact", contact, sizeof(contact)), TAG, "copy Contact failed");

    const char *uri = strcasestr_ascii(contact, "sip:");
    ESP_RETURN_ON_FALSE(uri != NULL, ESP_ERR_NOT_FOUND, TAG, "Contact SIP URI not found");

    const char *end = uri;
    while (*end != '\0' && *end != '>' && *end != ';' && *end != ' ' && *end != '\t') {
        ++end;
    }

    size_t len = (size_t)(end - uri);
    ESP_RETURN_ON_FALSE(len > 0 && len < out_len, ESP_ERR_INVALID_SIZE, TAG, "Contact SIP URI too large");
    memcpy(out, uri, len);
    out[len] = '\0';
    return ESP_OK;
}

static esp_err_t send_sip_response(int sock,
                                   const struct sockaddr_storage *dest_addr,
                                   socklen_t dest_addr_len,
                                   int code,
                                   const char *reason,
                                   const char *via,
                                   const char *from,
                                   const char *to,
                                   const char *call_id,
                                   const char *cseq,
                                   const char *body)
{
    const char *body_text = body == NULL ? "" : body;
    const char *content_type = body_text[0] == '\0' ? "" : "Content-Type: application/sdp\r\n";

    char response[1280] = {0};
    int response_len = snprintf(response,
                                sizeof(response),
                                "SIP/2.0 %d %s\r\n"
                                "Via: %s\r\n"
                                "From: %s\r\n"
                                "To: %s\r\n"
                                "Call-ID: %s\r\n"
                                "CSeq: %s\r\n"
                                "User-Agent: esp32-ipphone\r\n"
                                "%s"
                                "Content-Length: %u\r\n"
                                "\r\n"
                                "%s",
                                code,
                                reason,
                                via,
                                from,
                                to,
                                call_id,
                                cseq,
                                content_type,
                                (unsigned)strlen(body_text),
                                body_text);
    ESP_RETURN_ON_FALSE(response_len > 0 && response_len < (int)sizeof(response),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "SIP response too large");

    const bool lock_shared_socket = sock == s_incoming_sock && s_sip_io_mutex != NULL;
    if (lock_shared_socket) {
        ESP_RETURN_ON_FALSE(xSemaphoreTake(s_sip_io_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
                            ESP_ERR_TIMEOUT,
                            TAG,
                            "take SIP socket mutex for response failed");
    }

    int sent = sendto(sock, response, response_len, 0, (const struct sockaddr *)dest_addr, dest_addr_len);
    if (lock_shared_socket) {
        xSemaphoreGive(s_sip_io_mutex);
    }

    ESP_RETURN_ON_FALSE(sent >= 0, ESP_FAIL, TAG, "send SIP response failed: errno=%d", errno);
    ESP_LOGI(TAG, "sent SIP response %d %s", code, reason);
    return ESP_OK;
}

static esp_err_t send_incoming_response(int code, const char *reason, const char *body)
{
    ESP_RETURN_ON_FALSE(s_incoming_call.active, ESP_ERR_INVALID_STATE, TAG, "no active incoming call");
    ESP_RETURN_ON_FALSE(s_incoming_sock >= 0, ESP_ERR_INVALID_STATE, TAG, "incoming SIP socket is not ready");

    esp_err_t err = send_sip_response(s_incoming_sock,
                                      &s_incoming_call.remote_addr,
                                      s_incoming_call.remote_addr_len,
                                      code,
                                      reason,
                                      s_incoming_call.via,
                                      s_incoming_call.from,
                                      s_incoming_call.to,
                                      s_incoming_call.call_id,
                                      s_incoming_call.cseq,
                                      body);
    return err;
}

static esp_err_t send_in_call_bye(void)
{
    ESP_RETURN_ON_FALSE(s_incoming_call.active, ESP_ERR_INVALID_STATE, TAG, "no active call for BYE");
    ESP_RETURN_ON_FALSE(s_incoming_sock >= 0, ESP_ERR_INVALID_STATE, TAG, "incoming SIP socket is not ready");

    char local_ip[16] = {0};
    esp_err_t err = get_local_ip_string(local_ip, sizeof(local_ip));
    if (err != ESP_OK) {
        strlcpy(local_ip, CONFIG_IPPHONE_WIFI_HOSTNAME, sizeof(local_ip));
    }

    char branch[40] = {0};
    snprintf(branch, sizeof(branch), "z9hG4bK-%08lx", (unsigned long)esp_random());

    char remote_user[64] = {0};
    err = copy_sip_uri_user(s_incoming_call.from, remote_user, sizeof(remote_user));
    if (err != ESP_OK) {
        strlcpy(remote_user, CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION, sizeof(remote_user));
    }

    char request[1024] = {0};
    int request_len = snprintf(request,
                               sizeof(request),
                               "BYE sip:%s@%s SIP/2.0\r\n"
                               "Via: SIP/2.0/%s %s:%d;branch=%s;rport\r\n"
                               "Max-Forwards: 70\r\n"
                               "From: %s\r\n"
                               "To: %s\r\n"
                               "Call-ID: %s\r\n"
                               "CSeq: %lu BYE\r\n"
                               "User-Agent: esp32-ipphone\r\n"
                               "Content-Length: 0\r\n"
                               "\r\n",
                               remote_user,
                               CONFIG_IPPHONE_SIP_SERVER,
                               SIP_UDP_TRANSPORT,
                               local_ip,
                               CONFIG_IPPHONE_SIP_PORT,
                               branch,
                               s_incoming_call.to,
                               s_incoming_call.from,
                               s_incoming_call.call_id,
                               (unsigned long)s_dialog_cseq++);
    ESP_RETURN_ON_FALSE(request_len > 0 && request_len < (int)sizeof(request),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "SIP BYE request too large");

    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_sip_io_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "take SIP socket mutex for BYE failed");
    int sent = sendto(s_incoming_sock,
                      request,
                      request_len,
                      0,
                      (const struct sockaddr *)&s_incoming_call.remote_addr,
                      s_incoming_call.remote_addr_len);
    xSemaphoreGive(s_sip_io_mutex);

    ESP_RETURN_ON_FALSE(sent >= 0, ESP_FAIL, TAG, "send SIP BYE failed: errno=%d", errno);
    ESP_LOGI(TAG, "sent SIP BYE");
    return ESP_OK;
}

static esp_err_t wait_for_sip_final_response(const char *method,
                                             const char *call_id,
                                             char *response,
                                             size_t response_len,
                                             int timeout_ms,
                                             int *status_code)
{
    ESP_RETURN_ON_FALSE(method != NULL && call_id != NULL && response != NULL && status_code != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid SIP wait args");

    *status_code = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        const TickType_t remaining = deadline - xTaskGetTickCount();
        const TickType_t wait_ticks = remaining > pdMS_TO_TICKS(SIP_RX_TIMEOUT_MS) ? pdMS_TO_TICKS(SIP_RX_TIMEOUT_MS) : remaining;

        sip_response_msg_t queued_response = {0};
        if (xQueueReceive(s_sip_response_queue, &queued_response, wait_ticks) != pdTRUE) {
            continue;
        }

        size_t received = queued_response.len;
        if (received >= response_len) {
            received = response_len - 1;
        }
        memcpy(response, queued_response.data, received);
        response[received] = '\0';

        if (!sip_response_matches_transaction(response, method, call_id)) {
            ESP_LOGI(TAG, "queued SIP response ignored while waiting for %s", method);
            continue;
        }

        *status_code = parse_sip_status_code(response);
        ESP_LOGI(TAG, "SIP %s response: %d", method, *status_code);
        if (*status_code >= 200) {
            return ESP_OK;
        }

        ESP_LOGI(TAG, "SIP %s provisional response ignored", method);
    }

    ESP_LOGE(TAG, "SIP %s final response timeout", method);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t send_outgoing_ack(const struct sockaddr_storage *dest_addr,
                                   socklen_t dest_addr_len,
                                   const char *request_uri,
                                   const char *call_id,
                                   uint32_t invite_cseq,
                                   const char *from,
                                   const char *to)
{
    char local_ip[16] = {0};
    esp_err_t err = get_local_ip_string(local_ip, sizeof(local_ip));
    if (err != ESP_OK) {
        strlcpy(local_ip, CONFIG_IPPHONE_WIFI_HOSTNAME, sizeof(local_ip));
    }

    char branch[40] = {0};
    snprintf(branch, sizeof(branch), "z9hG4bK-%08lx", (unsigned long)esp_random());

    char request[1024] = {0};
    int request_len = snprintf(request,
                               sizeof(request),
                               "ACK %s SIP/2.0\r\n"
                               "Via: SIP/2.0/%s %s:%d;branch=%s;rport\r\n"
                               "Max-Forwards: 70\r\n"
                               "From: %s\r\n"
                               "To: %s\r\n"
                               "Call-ID: %s\r\n"
                               "CSeq: %lu ACK\r\n"
                               "User-Agent: esp32-ipphone\r\n"
                               "Content-Length: 0\r\n"
                               "\r\n",
                               request_uri,
                               SIP_UDP_TRANSPORT,
                               local_ip,
                               CONFIG_IPPHONE_SIP_PORT,
                               branch,
                               from,
                               to,
                               call_id,
                               (unsigned long)invite_cseq);
    ESP_RETURN_ON_FALSE(request_len > 0 && request_len < (int)sizeof(request),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "SIP ACK request too large");

    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_sip_io_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "take SIP socket mutex for ACK failed");
    int sent = sendto(s_incoming_sock, request, request_len, 0, (const struct sockaddr *)dest_addr, dest_addr_len);
    xSemaphoreGive(s_sip_io_mutex);

    ESP_RETURN_ON_FALSE(sent >= 0, ESP_FAIL, TAG, "send SIP ACK failed: errno=%d", errno);
    ESP_LOGI(TAG, "sent SIP ACK to %s", request_uri);
    return ESP_OK;
}

static esp_err_t send_outgoing_error_ack(const struct sockaddr_storage *dest_addr,
                                         socklen_t dest_addr_len,
                                         const char *local_ip,
                                         const char *call_id,
                                         uint32_t invite_cseq,
                                         const char *branch,
                                         const char *from,
                                         const char *to)
{
    char request[1024] = {0};
    int request_len = snprintf(request,
                               sizeof(request),
                               "ACK sip:%s@%s SIP/2.0\r\n"
                               "Via: SIP/2.0/%s %s:%d;branch=%s;rport\r\n"
                               "Max-Forwards: 70\r\n"
                               "From: %s\r\n"
                               "To: %s\r\n"
                               "Call-ID: %s\r\n"
                               "CSeq: %lu ACK\r\n"
                               "User-Agent: esp32-ipphone\r\n"
                               "Content-Length: 0\r\n"
                               "\r\n",
                               CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION,
                               CONFIG_IPPHONE_SIP_SERVER,
                               SIP_UDP_TRANSPORT,
                               local_ip,
                               CONFIG_IPPHONE_SIP_PORT,
                               branch,
                               from,
                               to,
                               call_id,
                               (unsigned long)invite_cseq);
    ESP_RETURN_ON_FALSE(request_len > 0 && request_len < (int)sizeof(request),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "SIP error ACK request too large");

    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_sip_io_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "take SIP socket mutex for error ACK failed");
    int sent = sendto(s_incoming_sock, request, request_len, 0, (const struct sockaddr *)dest_addr, dest_addr_len);
    xSemaphoreGive(s_sip_io_mutex);

    ESP_RETURN_ON_FALSE(sent >= 0, ESP_FAIL, TAG, "send SIP error ACK failed: errno=%d", errno);
    ESP_LOGI(TAG, "sent SIP ACK for INVITE final response");
    return ESP_OK;
}

static esp_err_t send_outgoing_invite_request(const struct sockaddr_storage *dest_addr,
                                              socklen_t dest_addr_len,
                                              const char *local_ip,
                                              const char *sdp,
                                              const char *call_id,
                                              const char *branch,
                                              const char *from,
                                              const char *to,
                                              uint32_t cseq,
                                              const char *authorization)
{
    char auth_line[640] = {0};
    if (authorization != NULL && authorization[0] != '\0') {
        int auth_len = snprintf(auth_line, sizeof(auth_line), "%s\r\n", authorization);
        ESP_RETURN_ON_FALSE(auth_len > 0 && auth_len < (int)sizeof(auth_line),
                            ESP_ERR_INVALID_SIZE,
                            TAG,
                            "SIP INVITE Authorization line too large");
    }

    char request[1792] = {0};
    int request_len = snprintf(request,
                               sizeof(request),
                               "INVITE sip:%s@%s SIP/2.0\r\n"
                               "Via: SIP/2.0/%s %s:%d;branch=%s;rport\r\n"
                               "Max-Forwards: 70\r\n"
                               "From: %s\r\n"
                               "To: %s\r\n"
                               "Call-ID: %s\r\n"
                               "CSeq: %lu INVITE\r\n"
                               "Contact: <sip:%s@%s:%d;transport=udp>\r\n"
                               "User-Agent: esp32-ipphone\r\n"
                               "%s"
                               "Content-Type: application/sdp\r\n"
                               "Content-Length: %u\r\n"
                               "\r\n"
                               "%s",
                               CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION,
                               CONFIG_IPPHONE_SIP_SERVER,
                               SIP_UDP_TRANSPORT,
                               local_ip,
                               CONFIG_IPPHONE_SIP_PORT,
                               branch,
                               from,
                               to,
                               call_id,
                               (unsigned long)cseq,
                               CONFIG_IPPHONE_SIP_USER,
                               local_ip,
                               CONFIG_IPPHONE_SIP_PORT,
                               auth_line,
                               (unsigned)strlen(sdp),
                               sdp);
    ESP_RETURN_ON_FALSE(request_len > 0 && request_len < (int)sizeof(request),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "SIP INVITE request too large");

    if (xSemaphoreTake(s_sip_io_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "take SIP socket mutex for INVITE failed");
        return ESP_ERR_TIMEOUT;
    }
    int sent = sendto(s_incoming_sock, request, request_len, 0, (const struct sockaddr *)dest_addr, dest_addr_len);
    xSemaphoreGive(s_sip_io_mutex);

    ESP_RETURN_ON_FALSE(sent >= 0, ESP_FAIL, TAG, "send SIP INVITE failed: errno=%d", errno);
    return ESP_OK;
}

static void remember_outgoing_invite_transaction(const struct sockaddr_storage *dest_addr,
                                                 socklen_t dest_addr_len,
                                                 const char *local_ip,
                                                 const char *call_id,
                                                 const char *branch,
                                                 const char *from,
                                                 const char *to,
                                                 uint32_t cseq)
{
    memcpy(&s_outgoing_dest_addr, dest_addr, dest_addr_len);
    s_outgoing_dest_addr_len = dest_addr_len;
    strlcpy(s_outgoing_local_ip, local_ip, sizeof(s_outgoing_local_ip));
    strlcpy(s_outgoing_call_id, call_id, sizeof(s_outgoing_call_id));
    strlcpy(s_outgoing_branch, branch, sizeof(s_outgoing_branch));
    strlcpy(s_outgoing_from, from, sizeof(s_outgoing_from));
    strlcpy(s_outgoing_to, to, sizeof(s_outgoing_to));
    s_outgoing_invite_cseq = cseq;
    s_outgoing_invite_active = true;
}

static esp_err_t send_outgoing_cancel(void)
{
    ESP_RETURN_ON_FALSE(s_outgoing_invite_active, ESP_ERR_INVALID_STATE, TAG, "no active outgoing INVITE to cancel");
    ESP_RETURN_ON_FALSE(s_incoming_sock >= 0, ESP_ERR_INVALID_STATE, TAG, "incoming SIP socket is not ready");

    char request[1024] = {0};
    int request_len = snprintf(request,
                               sizeof(request),
                               "CANCEL sip:%s@%s SIP/2.0\r\n"
                               "Via: SIP/2.0/%s %s:%d;branch=%s;rport\r\n"
                               "Max-Forwards: 70\r\n"
                               "From: %s\r\n"
                               "To: %s\r\n"
                               "Call-ID: %s\r\n"
                               "CSeq: %lu CANCEL\r\n"
                               "User-Agent: esp32-ipphone\r\n"
                               "Content-Length: 0\r\n"
                               "\r\n",
                               CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION,
                               CONFIG_IPPHONE_SIP_SERVER,
                               SIP_UDP_TRANSPORT,
                               s_outgoing_local_ip,
                               CONFIG_IPPHONE_SIP_PORT,
                               s_outgoing_branch,
                               s_outgoing_from,
                               s_outgoing_to,
                               s_outgoing_call_id,
                               (unsigned long)s_outgoing_invite_cseq);
    ESP_RETURN_ON_FALSE(request_len > 0 && request_len < (int)sizeof(request),
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "SIP CANCEL request too large");

    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_sip_io_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "take SIP socket mutex for CANCEL failed");
    int sent = sendto(s_incoming_sock,
                      request,
                      request_len,
                      0,
                      (const struct sockaddr *)&s_outgoing_dest_addr,
                      s_outgoing_dest_addr_len);
    xSemaphoreGive(s_sip_io_mutex);

    ESP_RETURN_ON_FALSE(sent >= 0, ESP_FAIL, TAG, "send SIP CANCEL failed: errno=%d", errno);
    ESP_LOGI(TAG, "sent SIP CANCEL");
    return ESP_OK;
}

static esp_err_t send_outgoing_invite(void)
{
    ESP_RETURN_ON_FALSE(strlen(CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION) > 0,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "default outgoing extension is empty");
    ESP_RETURN_ON_FALSE(s_incoming_sock >= 0, ESP_ERR_INVALID_STATE, TAG, "incoming SIP socket is not ready");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_sip_transaction_mutex, pdMS_TO_TICKS(SIP_INVITE_RESPONSE_TIMEOUT_MS + 2000)) ==
                            pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "take SIP transaction mutex for INVITE failed");
    s_outgoing_cancel_requested = false;
    s_outgoing_invite_active = false;
    s_state = SIP_PHONE_STATE_CALLING;

    struct sockaddr_storage dest_addr = {0};
    socklen_t dest_addr_len = 0;
    esp_err_t err = resolve_sip_server(&dest_addr, &dest_addr_len);
    if (err != ESP_OK) {
        s_outgoing_invite_active = false;
        s_state = SIP_PHONE_STATE_REGISTERED;
        xSemaphoreGive(s_sip_transaction_mutex);
        return err;
    }

    char local_ip[16] = {0};
    err = get_local_ip_string(local_ip, sizeof(local_ip));
    if (err != ESP_OK) {
        strlcpy(local_ip, CONFIG_IPPHONE_WIFI_HOSTNAME, sizeof(local_ip));
    }

    char sdp[512] = {0};
    err = build_basic_sdp(sdp, sizeof(sdp));
    if (err != ESP_OK) {
        s_outgoing_invite_active = false;
        s_state = SIP_PHONE_STATE_REGISTERED;
        xSemaphoreGive(s_sip_transaction_mutex);
        return err;
    }

    char call_id[48] = {0};
    char from_tag[16] = {0};
    char branch[40] = {0};
    snprintf(call_id, sizeof(call_id), "%08lx@esp32-ipphone", (unsigned long)esp_random());
    snprintf(from_tag, sizeof(from_tag), "%08lx", (unsigned long)esp_random());
    snprintf(branch, sizeof(branch), "z9hG4bK-%08lx", (unsigned long)esp_random());

    char from[256] = {0};
    char to[256] = {0};
    snprintf(from, sizeof(from), "<sip:%s@%s>;tag=%s", CONFIG_IPPHONE_SIP_USER, CONFIG_IPPHONE_SIP_SERVER, from_tag);
    snprintf(to, sizeof(to), "<sip:%s@%s>", CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION, CONFIG_IPPHONE_SIP_SERVER);

    uint32_t invite_cseq = s_cseq++;
    xQueueReset(s_sip_response_queue);
    ESP_LOGI(TAG, "sending SIP INVITE to extension %s", CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION);
    remember_outgoing_invite_transaction(&dest_addr, dest_addr_len, local_ip, call_id, branch, from, to, invite_cseq);
    err = send_outgoing_invite_request(&dest_addr,
                                       dest_addr_len,
                                       local_ip,
                                       sdp,
                                       call_id,
                                       branch,
                                       from,
                                       to,
                                       invite_cseq,
                                       NULL);
    if (err != ESP_OK) {
        s_outgoing_invite_active = false;
        s_state = SIP_PHONE_STATE_REGISTERED;
        xSemaphoreGive(s_sip_transaction_mutex);
        return err;
    }
    esp_err_t ringback_err = start_ringback();
    if (ringback_err != ESP_OK) {
        ESP_LOGW(TAG, "start outgoing ringback failed: %s", esp_err_to_name(ringback_err));
    }

    int status_code = 0;
    memset(s_outgoing_response, 0, sizeof(s_outgoing_response));
    err = wait_for_sip_final_response("INVITE",
                                      call_id,
                                      s_outgoing_response,
                                      sizeof(s_outgoing_response),
                                      SIP_INVITE_RESPONSE_TIMEOUT_MS,
                                      &status_code);
    if (err != ESP_OK) {
        stop_ringback();
        s_outgoing_invite_active = false;
        if (s_outgoing_cancel_requested) {
            s_state = SIP_PHONE_STATE_REGISTERED;
            xSemaphoreGive(s_sip_transaction_mutex);
            ESP_LOGI(TAG, "outgoing call cancelled");
            return ESP_OK;
        }
        xSemaphoreGive(s_sip_transaction_mutex);
        return err;
    }

    if (s_outgoing_cancel_requested) {
        stop_ringback();
        s_outgoing_invite_active = false;
        s_state = SIP_PHONE_STATE_REGISTERED;
        xSemaphoreGive(s_sip_transaction_mutex);
        ESP_LOGI(TAG, "outgoing call cancelled");
        return ESP_OK;
    }

    if (status_code == 401 || status_code == 407) {
        char challenge_to[256] = {0};
        if (copy_sip_header_value(s_outgoing_response, "To", challenge_to, sizeof(challenge_to)) == ESP_OK) {
            esp_err_t ack_err = send_outgoing_error_ack(&dest_addr,
                                                       dest_addr_len,
                                                       local_ip,
                                                       call_id,
                                                       invite_cseq,
                                                       branch,
                                                       from,
                                                       challenge_to);
            if (ack_err != ESP_OK) {
                ESP_LOGW(TAG, "send ACK for INVITE challenge failed: %s", esp_err_to_name(ack_err));
            }
        }

        sip_digest_challenge_t challenge = {0};
        err = parse_digest_challenge(s_outgoing_response, status_code, &challenge);
        if (err != ESP_OK) {
            stop_ringback();
            s_outgoing_invite_active = false;
            s_state = SIP_PHONE_STATE_REGISTERED;
            xSemaphoreGive(s_sip_transaction_mutex);
            return err;
        }

        char cnonce[17] = {0};
        snprintf(cnonce, sizeof(cnonce), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());

        char invite_uri[96] = {0};
        snprintf(invite_uri,
                 sizeof(invite_uri),
                 "sip:%s@%s",
                 CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION,
                 CONFIG_IPPHONE_SIP_SERVER);

        char authorization[640] = {0};
        err = build_digest_authorization(&challenge,
                                         status_code,
                                         "INVITE",
                                         invite_uri,
                                         1,
                                         cnonce,
                                         authorization,
                                         sizeof(authorization));
        if (err != ESP_OK) {
            stop_ringback();
            s_outgoing_invite_active = false;
            s_state = SIP_PHONE_STATE_REGISTERED;
            xSemaphoreGive(s_sip_transaction_mutex);
            return err;
        }

        snprintf(branch, sizeof(branch), "z9hG4bK-%08lx", (unsigned long)esp_random());
        invite_cseq = s_cseq++;
        xQueueReset(s_sip_response_queue);
        ESP_LOGI(TAG, "SIP INVITE digest challenge received, sending authenticated INVITE");
        remember_outgoing_invite_transaction(&dest_addr, dest_addr_len, local_ip, call_id, branch, from, to, invite_cseq);
        err = send_outgoing_invite_request(&dest_addr,
                                           dest_addr_len,
                                           local_ip,
                                           sdp,
                                           call_id,
                                           branch,
                                           from,
                                           to,
                                           invite_cseq,
                                           authorization);
        if (err != ESP_OK) {
            stop_ringback();
            s_outgoing_invite_active = false;
            s_state = SIP_PHONE_STATE_REGISTERED;
            xSemaphoreGive(s_sip_transaction_mutex);
            return err;
        }

        memset(s_outgoing_response, 0, sizeof(s_outgoing_response));
        err = wait_for_sip_final_response("INVITE",
                                          call_id,
                                          s_outgoing_response,
                                          sizeof(s_outgoing_response),
                                          SIP_INVITE_RESPONSE_TIMEOUT_MS,
                                          &status_code);
        if (err != ESP_OK) {
            stop_ringback();
            s_outgoing_invite_active = false;
            if (s_outgoing_cancel_requested) {
                s_state = SIP_PHONE_STATE_REGISTERED;
                xSemaphoreGive(s_sip_transaction_mutex);
                ESP_LOGI(TAG, "outgoing call cancelled");
                return ESP_OK;
            }
            xSemaphoreGive(s_sip_transaction_mutex);
            return err;
        }
    }

    if (s_outgoing_cancel_requested) {
        stop_ringback();
        s_outgoing_invite_active = false;
        s_state = SIP_PHONE_STATE_REGISTERED;
        xSemaphoreGive(s_sip_transaction_mutex);
        ESP_LOGI(TAG, "outgoing call cancelled");
        return ESP_OK;
    }

    if (status_code != 200) {
        stop_ringback();
        char final_to[256] = {0};
        if (copy_sip_header_value(s_outgoing_response, "To", final_to, sizeof(final_to)) == ESP_OK) {
            esp_err_t ack_err = send_outgoing_error_ack(&dest_addr,
                                                       dest_addr_len,
                                                       local_ip,
                                                       call_id,
                                                       invite_cseq,
                                                       branch,
                                                       from,
                                                       final_to);
            if (ack_err != ESP_OK) {
                ESP_LOGW(TAG, "send ACK for rejected INVITE failed: %s", esp_err_to_name(ack_err));
            }
        }
        xSemaphoreGive(s_sip_transaction_mutex);
        ESP_LOGW(TAG, "outgoing INVITE rejected with status %d", status_code);
        s_outgoing_invite_active = false;
        s_state = SIP_PHONE_STATE_REGISTERED;
        return ESP_ERR_INVALID_RESPONSE;
    }

    char response_to[256] = {0};
    err = copy_sip_header_value(s_outgoing_response, "To", response_to, sizeof(response_to));
    if (err != ESP_OK) {
        stop_ringback();
        s_outgoing_invite_active = false;
        s_state = SIP_PHONE_STATE_REGISTERED;
        xSemaphoreGive(s_sip_transaction_mutex);
        return err;
    }

    char ack_uri[128] = {0};
    err = copy_sip_contact_uri(s_outgoing_response, ack_uri, sizeof(ack_uri));
    if (err != ESP_OK) {
        snprintf(ack_uri,
                 sizeof(ack_uri),
                 "sip:%s@%s",
                 CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION,
                 CONFIG_IPPHONE_SIP_SERVER);
        ESP_LOGW(TAG, "Contact missing in 200 OK, using fallback ACK URI %s", ack_uri);
    }

    err = send_outgoing_ack(&dest_addr, dest_addr_len, ack_uri, call_id, invite_cseq, from, response_to);
    if (err != ESP_OK) {
        stop_ringback();
        s_outgoing_invite_active = false;
        s_state = SIP_PHONE_STATE_REGISTERED;
        xSemaphoreGive(s_sip_transaction_mutex);
        return err;
    }
    stop_ringback();
    s_outgoing_invite_active = false;

    memset(&s_incoming_call, 0, sizeof(s_incoming_call));
    memcpy(&s_incoming_call.remote_addr, &dest_addr, dest_addr_len);
    s_incoming_call.remote_addr_len = dest_addr_len;
    strlcpy(s_incoming_call.to, from, sizeof(s_incoming_call.to));
    strlcpy(s_incoming_call.from, response_to, sizeof(s_incoming_call.from));
    strlcpy(s_incoming_call.call_id, call_id, sizeof(s_incoming_call.call_id));
    snprintf(s_incoming_call.cseq, sizeof(s_incoming_call.cseq), "%lu INVITE", (unsigned long)invite_cseq);
    strlcpy(s_incoming_call.remote_sdp, sip_message_body(s_outgoing_response), sizeof(s_incoming_call.remote_sdp));
    s_incoming_call.active = true;

    xSemaphoreGive(s_sip_transaction_mutex);
    return ESP_OK;
}

static esp_err_t build_basic_sdp(char *out, size_t out_len)
{
    char local_ip[16] = {0};
    ESP_RETURN_ON_ERROR(get_local_ip_string(local_ip, sizeof(local_ip)), TAG, "get local IP for SDP failed");

    int len = snprintf(out,
                       out_len,
                       "v=0\r\n"
                       "o=esp32-ipphone 0 0 IN IP4 %s\r\n"
                       "s=esp32-ipphone\r\n"
                       "c=IN IP4 %s\r\n"
                       "t=0 0\r\n"
                       "m=audio %d RTP/AVP %d\r\n"
                       "a=rtpmap:%d %s/%d\r\n"
                       "a=sendrecv\r\n",
                       local_ip,
                       local_ip,
                       SIP_LOCAL_RTP_PORT,
                       SIP_AUDIO_CODEC_PAYLOAD,
                       SIP_AUDIO_CODEC_PAYLOAD,
                       SIP_AUDIO_CODEC_NAME,
                       SIP_AUDIO_CODEC_RATE);
    ESP_RETURN_ON_FALSE(len > 0 && len < (int)out_len, ESP_ERR_INVALID_SIZE, TAG, "SDP too large");
    return ESP_OK;
}

static int16_t alaw_to_pcm16(uint8_t alaw)
{
    alaw ^= 0x55;

    int16_t sample = (int16_t)((alaw & 0x0f) << 4);
    int segment = (alaw & 0x70) >> 4;

    switch (segment) {
    case 0:
        sample += 8;
        break;
    case 1:
        sample += 0x108;
        break;
    default:
        sample += 0x108;
        sample <<= (segment - 1);
        break;
    }

    return (alaw & 0x80) ? sample : (int16_t)-sample;
}

static int alaw_segment(int16_t value)
{
    for (int segment = 0; segment < 8; ++segment) {
        if (value <= (0x1f << segment)) {
            return segment;
        }
    }
    return 8;
}

static uint8_t pcm16_to_alaw(int16_t pcm)
{
    int16_t sample = pcm;
    uint8_t mask = 0xd5;

    if (sample < 0) {
        sample = (int16_t)-sample;
        mask = 0x55;
    }

    if (sample > 32635) {
        sample = 32635;
    }

    sample = (int16_t)(sample >> 3);
    const int segment = alaw_segment(sample);
    uint8_t alaw = 0x7f;
    if (segment < 8) {
        alaw = (uint8_t)((segment << 4) | ((sample >> (segment == 0 ? 1 : segment)) & 0x0f));
    }

    return alaw ^ mask;
}

static size_t rtp_header_length(const uint8_t *packet, size_t packet_len)
{
    if (packet_len < 12 || (packet[0] >> 6) != 2) {
        return 0;
    }

    size_t header_len = 12 + ((size_t)(packet[0] & 0x0f) * 4);
    if ((packet[0] & 0x10) != 0) {
        if (packet_len < header_len + 4) {
            return 0;
        }
        uint16_t extension_words = ((uint16_t)packet[header_len + 2] << 8) | packet[header_len + 3];
        header_len += 4 + ((size_t)extension_words * 4);
    }

    return header_len <= packet_len ? header_len : 0;
}

static void rtp_rx_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "create RTP RX socket failed: errno=%d", errno);
        s_rtp_rx_running = false;
        s_rtp_rx_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SIP_LOCAL_RTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "bind RTP RX socket failed: errno=%d", errno);
        close(sock);
        s_rtp_rx_running = false;
        s_rtp_rx_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    set_socket_rx_timeout(sock, RTP_RX_TIMEOUT_MS);

    ESP_LOGI(TAG, "RTP RX started on UDP port %d, codec=%s/%d", SIP_LOCAL_RTP_PORT, SIP_AUDIO_CODEC_NAME, SIP_AUDIO_CODEC_RATE);

    bool first_packet_logged = false;
    uint8_t packet[RTP_PACKET_BUFFER_BYTES] = {0};
    rtp_audio_frame_t frame = {0};
    rtp_audio_frame_t dropped_frame = {0};
    uint32_t dropped_count = 0;

    while (s_rtp_rx_running) {
        struct sockaddr_storage remote_addr = {0};
        socklen_t remote_addr_len = sizeof(remote_addr);
        int received = recvfrom(sock, packet, sizeof(packet), 0, (struct sockaddr *)&remote_addr, &remote_addr_len);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGW(TAG, "RTP RX recv failed: errno=%d", errno);
            continue;
        }

        const size_t packet_len = (size_t)received;
        const size_t header_len = rtp_header_length(packet, packet_len);
        if (header_len == 0 || header_len >= packet_len) {
            ESP_LOGW(TAG, "bad RTP packet len=%u", (unsigned)packet_len);
            continue;
        }

        const uint8_t payload_type = packet[1] & 0x7f;
        if (payload_type != SIP_AUDIO_CODEC_PAYLOAD) {
            ESP_LOGW(TAG, "unsupported RTP payload type=%u", payload_type);
            continue;
        }

        if (!first_packet_logged) {
            ESP_LOGI(TAG, "first RTP audio packet received, payload bytes=%u", (unsigned)(packet_len - header_len));
            first_packet_logged = true;
        }

        const uint8_t *payload = &packet[header_len];
        const size_t payload_len = packet_len - header_len;
        frame.sample_count = 0;

        for (size_t i = 0; i < payload_len && frame.sample_count < RTP_MAX_AUDIO_SAMPLES; ++i) {
            const int16_t sample = alaw_to_pcm16(payload[i]);
            frame.samples[frame.sample_count++] = sample;

            if (CONFIG_IPPHONE_I2S_SAMPLE_RATE >= (SIP_AUDIO_CODEC_RATE * 2) && frame.sample_count < RTP_MAX_AUDIO_SAMPLES) {
                frame.samples[frame.sample_count++] = sample;
            }
        }

        if (s_rtp_audio_queue != NULL && xQueueSend(s_rtp_audio_queue, &frame, 0) != pdTRUE) {
            xQueueReceive(s_rtp_audio_queue, &dropped_frame, 0);
            xQueueSend(s_rtp_audio_queue, &frame, 0);
            ++dropped_count;
            if ((dropped_count % 25) == 1) {
                ESP_LOGW(TAG, "RTP audio queue full, dropped frames=%lu", (unsigned long)dropped_count);
            }
        }
    }

    close(sock);

    ESP_LOGI(TAG, "RTP RX stopped");
    s_rtp_rx_task = NULL;
    vTaskDelete(NULL);
}

static void rtp_audio_task(void *arg)
{
    (void)arg;

    esp_err_t err = audio_hw_speaker_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start RTP speaker failed: %s", esp_err_to_name(err));
        s_rtp_rx_running = false;
        s_rtp_audio_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    rtp_audio_frame_t frame = {0};
    int16_t silence[RTP_MAX_AUDIO_SAMPLES] = {0};
    size_t silence_count = (CONFIG_IPPHONE_I2S_SAMPLE_RATE * RTP_AUDIO_FRAME_MS) / 1000;
    if (silence_count > RTP_MAX_AUDIO_SAMPLES) {
        silence_count = RTP_MAX_AUDIO_SAMPLES;
    }
    bool first_frame_played = false;

    while (s_rtp_rx_running) {
        if (s_rtp_audio_queue != NULL && xQueueReceive(s_rtp_audio_queue, &frame, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (!first_frame_played) {
                ESP_LOGI(TAG, "RTP audio playback started, samples=%u", (unsigned)frame.sample_count);
                first_frame_played = true;
            }
            call_recorder_push_audio(CALL_RECORDER_AUDIO_RX, frame.samples, frame.sample_count);
            err = audio_hw_speaker_write(frame.samples, frame.sample_count);
        } else if (first_frame_played) {
            err = audio_hw_speaker_write(silence, silence_count);
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RTP speaker write failed: %s", esp_err_to_name(err));
            break;
        }
    }

    esp_err_t stop_err = audio_hw_speaker_stop();
    if (stop_err != ESP_OK) {
        ESP_LOGW(TAG, "stop RTP speaker failed: %s", esp_err_to_name(stop_err));
    }

    ESP_LOGI(TAG, "RTP audio stopped");
    s_rtp_audio_task = NULL;
    vTaskDelete(NULL);
}

static void rtp_tx_task(void *arg)
{
    (void)arg;

    struct sockaddr_storage remote_addr = {0};
    socklen_t remote_addr_len = 0;
    uint16_t remote_port = 0;
    esp_err_t err = parse_remote_rtp_endpoint(s_incoming_call.remote_sdp, &remote_addr, &remote_addr_len, &remote_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "parse remote RTP endpoint failed: %s", esp_err_to_name(err));
        s_rtp_tx_running = false;
        s_rtp_tx_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int sock = socket(remote_addr.ss_family, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "create RTP TX socket failed: errno=%d", errno);
        s_rtp_tx_running = false;
        s_rtp_tx_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    err = audio_hw_mic_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start RTP mic failed: %s", esp_err_to_name(err));
        close(sock);
        s_rtp_tx_running = false;
        s_rtp_tx_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t packet[RTP_HEADER_BYTES + RTP_TX_PAYLOAD_BYTES] = {0};
    int16_t samples[RTP_TX_PAYLOAD_BYTES] = {0};
    uint16_t sequence = (uint16_t)esp_random();
    uint32_t timestamp = esp_random();
    const uint32_t ssrc = esp_random();
    uint32_t sent_count = 0;
    uint32_t send_fail_count = 0;
    TickType_t next_frame_tick = xTaskGetTickCount();

    ESP_LOGI(TAG, "RTP TX started to remote audio port %u, codec=%s/%d", remote_port, SIP_AUDIO_CODEC_NAME, SIP_AUDIO_CODEC_RATE);

    while (s_rtp_tx_running) {
        size_t samples_read = 0;
        int32_t peak = 0;
        err = audio_hw_mic_read(samples, RTP_TX_PAYLOAD_BYTES, &samples_read, &peak);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RTP mic read failed: %s", esp_err_to_name(err));
            break;
        }

        if (samples_read == 0) {
            continue;
        }

        if (samples_read > RTP_TX_PAYLOAD_BYTES) {
            samples_read = RTP_TX_PAYLOAD_BYTES;
        }

        int16_t recorded_samples[RTP_TX_PAYLOAD_BYTES] = {0};
        for (size_t i = 0; i < samples_read; ++i) {
            recorded_samples[i] = (int16_t)(samples[i] >> RTP_MIC_ATTENUATION_SHIFT);
        }
        call_recorder_push_audio(CALL_RECORDER_AUDIO_TX, recorded_samples, samples_read);

        packet[0] = 0x80;
        packet[1] = SIP_AUDIO_CODEC_PAYLOAD;
        packet[2] = (uint8_t)(sequence >> 8);
        packet[3] = (uint8_t)(sequence & 0xff);
        packet[4] = (uint8_t)(timestamp >> 24);
        packet[5] = (uint8_t)(timestamp >> 16);
        packet[6] = (uint8_t)(timestamp >> 8);
        packet[7] = (uint8_t)(timestamp & 0xff);
        packet[8] = (uint8_t)(ssrc >> 24);
        packet[9] = (uint8_t)(ssrc >> 16);
        packet[10] = (uint8_t)(ssrc >> 8);
        packet[11] = (uint8_t)(ssrc & 0xff);

        for (size_t i = 0; i < samples_read; ++i) {
            packet[RTP_HEADER_BYTES + i] = pcm16_to_alaw(recorded_samples[i]);
        }

        const size_t packet_len = RTP_HEADER_BYTES + samples_read;
        const int sent = sendto(sock, packet, packet_len, 0, (const struct sockaddr *)&remote_addr, remote_addr_len);
        if (sent < 0) {
            ++send_fail_count;
            if ((send_fail_count % 25) == 1) {
                ESP_LOGW(TAG,
                         "RTP TX send failed: errno=%d, failures=%lu",
                         errno,
                         (unsigned long)send_fail_count);
            }
            if (errno == ENOMEM) {
                vTaskDelay(pdMS_TO_TICKS(RTP_TX_SEND_BACKOFF_MS));
            }
        } else if (sent_count == 0) {
            ESP_LOGI(TAG, "first RTP microphone packet sent, payload bytes=%u, mic peak=%ld", (unsigned)samples_read, (long)peak);
        }

        ++sent_count;
        ++sequence;
        timestamp += (uint32_t)samples_read;
        vTaskDelayUntil(&next_frame_tick, pdMS_TO_TICKS(RTP_AUDIO_FRAME_MS));
    }

    esp_err_t stop_err = audio_hw_mic_stop();
    if (stop_err != ESP_OK) {
        ESP_LOGW(TAG, "stop RTP mic failed: %s", esp_err_to_name(stop_err));
    }

    close(sock);
    ESP_LOGI(TAG, "RTP TX stopped");
    s_rtp_tx_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_rtp_rx(void)
{
    if (s_rtp_rx_task != NULL || s_rtp_audio_task != NULL) {
        return ESP_OK;
    }

    if (s_rtp_audio_queue == NULL) {
        s_rtp_audio_queue = xQueueCreate(RTP_AUDIO_QUEUE_LENGTH, sizeof(rtp_audio_frame_t));
        ESP_RETURN_ON_FALSE(s_rtp_audio_queue != NULL, ESP_ERR_NO_MEM, TAG, "create RTP audio queue failed");
    }
    xQueueReset(s_rtp_audio_queue);

    s_rtp_rx_running = true;
    BaseType_t created = xTaskCreate(rtp_audio_task, "rtp_audio", RTP_RX_STACK_WORDS, NULL, RTP_AUDIO_TASK_PRIORITY, &s_rtp_audio_task);
    if (created != pdPASS) {
        s_rtp_rx_running = false;
        s_rtp_audio_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    created = xTaskCreate(rtp_rx_task, "rtp_rx", RTP_RX_STACK_WORDS, NULL, RTP_RX_TASK_PRIORITY, &s_rtp_rx_task);
    if (created != pdPASS) {
        s_rtp_rx_running = false;
        s_rtp_rx_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t start_rtp_tx(void)
{
    if (s_rtp_tx_task != NULL) {
        return ESP_OK;
    }

    s_rtp_tx_running = true;
    BaseType_t created = xTaskCreate(rtp_tx_task, "rtp_tx", RTP_TX_STACK_WORDS, NULL, RTP_TX_TASK_PRIORITY, &s_rtp_tx_task);
    if (created != pdPASS) {
        s_rtp_tx_running = false;
        s_rtp_tx_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void stop_rtp_rx(void)
{
    s_rtp_rx_running = false;
    s_rtp_tx_running = false;
    call_recorder_stop();
}

static void ring_signal_task(void *arg)
{
    (void)arg;

    esp_err_t err = audio_hw_speaker_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start ring speaker failed: %s", esp_err_to_name(err));
        s_ring_signal_running = false;
        s_ring_signal_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const size_t frame_samples = (CONFIG_IPPHONE_I2S_SAMPLE_RATE * RING_SIGNAL_FRAME_MS) / 1000;
    int16_t samples[CONFIG_IPPHONE_I2S_SAMPLE_RATE * RING_SIGNAL_FRAME_MS / 1000] = {0};
    uint32_t elapsed_ms = 0;
    uint32_t phase = 0;

    ESP_LOGI(TAG, "incoming ring signal started");

    while (s_ring_signal_running) {
        const uint32_t cycle_ms = RING_SIGNAL_BEEP_MS + RING_SIGNAL_PAUSE_MS;
        const bool beep = (elapsed_ms % cycle_ms) < RING_SIGNAL_BEEP_MS;

        for (size_t i = 0; i < frame_samples; ++i) {
            if (beep) {
                samples[i] = (phase++ & 0x04) ? RING_SIGNAL_AMPLITUDE : -RING_SIGNAL_AMPLITUDE;
            } else {
                samples[i] = 0;
                ++phase;
            }
        }

        err = audio_hw_speaker_write(samples, frame_samples);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ring speaker write failed: %s", esp_err_to_name(err));
            break;
        }

        elapsed_ms += RING_SIGNAL_FRAME_MS;
    }

    esp_err_t stop_err = audio_hw_speaker_stop();
    if (stop_err != ESP_OK) {
        ESP_LOGW(TAG, "stop ring speaker failed: %s", esp_err_to_name(stop_err));
    }

    ESP_LOGI(TAG, "incoming ring signal stopped");
    s_ring_signal_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_ring_signal(void)
{
    if (s_ring_signal_task != NULL) {
        return ESP_OK;
    }

    s_ring_signal_running = true;
    BaseType_t created = xTaskCreate(ring_signal_task,
                                     "ring_signal",
                                     SIP_RING_SIGNAL_STACK_WORDS,
                                     NULL,
                                     4,
                                     &s_ring_signal_task);
    if (created != pdPASS) {
        s_ring_signal_running = false;
        s_ring_signal_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void stop_ring_signal(void)
{
    s_ring_signal_running = false;
    for (int i = 0; i < 30 && s_ring_signal_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void ringback_task(void *arg)
{
    (void)arg;

    esp_err_t err = audio_hw_speaker_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start ringback speaker failed: %s", esp_err_to_name(err));
        s_ringback_running = false;
        s_ringback_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const size_t frame_samples = (CONFIG_IPPHONE_I2S_SAMPLE_RATE * RING_SIGNAL_FRAME_MS) / 1000;
    int16_t samples[CONFIG_IPPHONE_I2S_SAMPLE_RATE * RING_SIGNAL_FRAME_MS / 1000] = {0};
    uint32_t elapsed_ms = 0;
    uint32_t sample_index = 0;

    ESP_LOGI(TAG, "outgoing ringback 425 Hz started");

    while (s_ringback_running) {
        const uint32_t cycle_ms = RINGBACK_TONE_MS + RINGBACK_PAUSE_MS;
        const bool tone = (elapsed_ms % cycle_ms) < RINGBACK_TONE_MS;

        for (size_t i = 0; i < frame_samples; ++i) {
            if (tone) {
                const float phase = 2.0f * 3.14159265358979323846f * (float)RINGBACK_TONE_HZ *
                                    (float)sample_index / (float)CONFIG_IPPHONE_I2S_SAMPLE_RATE;
                samples[i] = (int16_t)(sinf(phase) * RINGBACK_AMPLITUDE);
            } else {
                samples[i] = 0;
            }
            ++sample_index;
        }

        err = audio_hw_speaker_write(samples, frame_samples);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ringback speaker write failed: %s", esp_err_to_name(err));
            break;
        }

        elapsed_ms += RING_SIGNAL_FRAME_MS;
    }

    esp_err_t stop_err = audio_hw_speaker_stop();
    if (stop_err != ESP_OK) {
        ESP_LOGW(TAG, "stop ringback speaker failed: %s", esp_err_to_name(stop_err));
    }

    ESP_LOGI(TAG, "outgoing ringback stopped");
    s_ringback_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_ringback(void)
{
    if (s_ringback_task != NULL) {
        return ESP_OK;
    }

    s_ringback_running = true;
    BaseType_t created = xTaskCreate(ringback_task, "ringback", SIP_RINGBACK_STACK_WORDS, NULL, 4, &s_ringback_task);
    if (created != pdPASS) {
        s_ringback_running = false;
        s_ringback_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void stop_ringback(void)
{
    s_ringback_running = false;
    for (int i = 0; i < 30 && s_ringback_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t handle_invite(int sock,
                               const char *message,
                               const struct sockaddr_storage *remote_addr,
                               socklen_t remote_addr_len)
{
    char via[256] = {0};
    char from[256] = {0};
    char to[256] = {0};
    char call_id[128] = {0};
    char cseq[96] = {0};
    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "Via", via, sizeof(via)), TAG, "copy INVITE Via failed");
    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "From", from, sizeof(from)), TAG, "copy INVITE From failed");
    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "To", to, sizeof(to)), TAG, "copy INVITE To failed");
    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "Call-ID", call_id, sizeof(call_id)), TAG, "copy INVITE Call-ID failed");
    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "CSeq", cseq, sizeof(cseq)), TAG, "copy INVITE CSeq failed");

    if (s_state == SIP_PHONE_STATE_RINGING || s_state == SIP_PHONE_STATE_IN_CALL) {
        bool same_dialog_invite = s_incoming_call.active &&
                                  strcmp(call_id, s_incoming_call.call_id) == 0 &&
                                  strcmp(cseq, s_incoming_call.cseq) == 0;
        if (same_dialog_invite && s_state == SIP_PHONE_STATE_RINGING) {
            ESP_LOGI(TAG, "retransmitted INVITE while ringing, resending 180 Ringing");
            return send_sip_response(sock,
                                     remote_addr,
                                     remote_addr_len,
                                     180,
                                     "Ringing",
                                     via,
                                     from,
                                     s_incoming_call.to,
                                     call_id,
                                     cseq,
                                     NULL);
        }

        if (same_dialog_invite && s_state == SIP_PHONE_STATE_IN_CALL) {
            char sdp[512] = {0};
            ESP_RETURN_ON_ERROR(build_basic_sdp(sdp, sizeof(sdp)), TAG, "build retransmit 200 OK SDP failed");
            ESP_LOGI(TAG, "retransmitted INVITE in call, resending 200 OK");
            return send_sip_response(sock,
                                     remote_addr,
                                     remote_addr_len,
                                     200,
                                     "OK",
                                     via,
                                     from,
                                     s_incoming_call.to,
                                     call_id,
                                     cseq,
                                     sdp);
        }

        return send_sip_response(sock, remote_addr, remote_addr_len, 486, "Busy Here", via, from, to, call_id, cseq, NULL);
    }

    memset(&s_incoming_call, 0, sizeof(s_incoming_call));
    memcpy(&s_incoming_call.remote_addr, remote_addr, remote_addr_len);
    s_incoming_call.remote_addr_len = remote_addr_len;
    snprintf(s_incoming_call.local_tag, sizeof(s_incoming_call.local_tag), "%08lx", (unsigned long)esp_random());

    strlcpy(s_incoming_call.via, via, sizeof(s_incoming_call.via));
    strlcpy(s_incoming_call.from, from, sizeof(s_incoming_call.from));
    strlcpy(s_incoming_call.to, to, sizeof(s_incoming_call.to));
    strlcpy(s_incoming_call.call_id, call_id, sizeof(s_incoming_call.call_id));
    strlcpy(s_incoming_call.cseq, cseq, sizeof(s_incoming_call.cseq));
    ESP_RETURN_ON_ERROR(format_to_with_tag(s_incoming_call.to,
                                           sizeof(s_incoming_call.to),
                                           s_incoming_call.to,
                                           s_incoming_call.local_tag),
                        TAG,
                        "tag INVITE To failed");

    strlcpy(s_incoming_call.remote_sdp, sip_message_body(message), sizeof(s_incoming_call.remote_sdp));
    if (!sdp_supports_g711a(s_incoming_call.remote_sdp)) {
        ESP_LOGW(TAG, "incoming INVITE does not offer G.711A/PCMA");
        return send_sip_response(sock,
                                 remote_addr,
                                 remote_addr_len,
                                 488,
                                 "Not Acceptable Here",
                                 s_incoming_call.via,
                                 s_incoming_call.from,
                                 s_incoming_call.to,
                                 s_incoming_call.call_id,
                                 s_incoming_call.cseq,
                                 NULL);
    }

    s_incoming_call.active = true;
    s_state = SIP_PHONE_STATE_RINGING;

    ESP_LOGI(TAG, "incoming SIP call: From=%s Call-ID=%s", s_incoming_call.from, s_incoming_call.call_id);
    esp_err_t ring_err = start_ring_signal();
    if (ring_err != ESP_OK) {
        ESP_LOGW(TAG, "start incoming ring signal failed: %s", esp_err_to_name(ring_err));
    }
    ESP_RETURN_ON_ERROR(send_incoming_response(100, "Trying", NULL), TAG, "send 100 Trying failed");
    return send_incoming_response(180, "Ringing", NULL);
}

static void clear_incoming_call(void)
{
    memset(&s_incoming_call, 0, sizeof(s_incoming_call));
}

static void start_call_recording_from_dialog(void)
{
    if (!call_recorder_is_ready() || !s_incoming_call.active) {
        return;
    }

    char caller[48] = {0};
    char callee[48] = {0};
    if (copy_sip_uri_user(s_incoming_call.from, caller, sizeof(caller)) != ESP_OK) {
        strlcpy(caller, "unknown", sizeof(caller));
    }
    if (copy_sip_uri_user(s_incoming_call.to, callee, sizeof(callee)) != ESP_OK) {
        strlcpy(callee, CONFIG_IPPHONE_SIP_USER, sizeof(callee));
    }

    esp_err_t err = call_recorder_start(caller, callee);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "call recording not started: %s", esp_err_to_name(err));
    }
}

static esp_err_t send_simple_response_for_message(int sock,
                                                  const char *message,
                                                  const struct sockaddr_storage *remote_addr,
                                                  socklen_t remote_addr_len,
                                                  int code,
                                                  const char *reason)
{
    char via[256] = {0};
    char from[256] = {0};
    char to[256] = {0};
    char call_id[128] = {0};
    char cseq[96] = {0};

    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "Via", via, sizeof(via)), TAG, "copy Via failed");
    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "From", from, sizeof(from)), TAG, "copy From failed");
    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "To", to, sizeof(to)), TAG, "copy To failed");
    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "Call-ID", call_id, sizeof(call_id)), TAG, "copy Call-ID failed");
    ESP_RETURN_ON_ERROR(copy_sip_header_value(message, "CSeq", cseq, sizeof(cseq)), TAG, "copy CSeq failed");

    return send_sip_response(sock, remote_addr, remote_addr_len, code, reason, via, from, to, call_id, cseq, NULL);
}

static void queue_sip_response_for_register(const char *message, int received)
{
    if (s_sip_response_queue == NULL || received <= 0) {
        return;
    }

    sip_response_msg_t response = {0};
    response.len = (size_t)received;
    if (response.len >= sizeof(response.data)) {
        response.len = sizeof(response.data) - 1;
    }
    memcpy(response.data, message, response.len);
    response.data[response.len] = '\0';

    if (xQueueSend(s_sip_response_queue, &response, 0) == pdTRUE) {
        return;
    }

    sip_response_msg_t dropped = {0};
    (void)xQueueReceive(s_sip_response_queue, &dropped, 0);
    if (xQueueSend(s_sip_response_queue, &response, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SIP response queue full, dropped response");
    }
}

static void incoming_sip_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "create incoming SIP socket failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    if (bind_socket_to_sip_port(sock) != ESP_OK) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    s_incoming_sock = sock;
    ESP_LOGI(TAG, "incoming SIP listener ready on UDP port %d", CONFIG_IPPHONE_SIP_PORT);

    while (true) {
        char message[1536] = {0};
        struct sockaddr_storage remote_addr = {0};
        socklen_t remote_addr_len = sizeof(remote_addr);

        if (xSemaphoreTake(s_sip_io_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        set_socket_rx_timeout(sock, SIP_INCOMING_RECV_TIMEOUT_MS);
        int received = recvfrom(sock,
                                message,
                                sizeof(message) - 1,
                                0,
                                (struct sockaddr *)&remote_addr,
                                &remote_addr_len);
        xSemaphoreGive(s_sip_io_mutex);
        vTaskDelay(1);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            ESP_LOGW(TAG, "incoming SIP recv failed: errno=%d", errno);
            continue;
        }

        message[received] = '\0';

        if (strncmp(message, "SIP/2.0", 7) == 0) {
            queue_sip_response_for_register(message, received);
        } else if (sip_message_starts_with_method(message, "INVITE")) {
            esp_err_t err = handle_invite(sock, message, &remote_addr, remote_addr_len);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "handle INVITE failed: %s", esp_err_to_name(err));
            }
        } else if (sip_message_starts_with_method(message, "CANCEL")) {
            ESP_LOGI(TAG, "incoming SIP CANCEL");
            send_simple_response_for_message(sock, message, &remote_addr, remote_addr_len, 200, "OK");
            if (s_incoming_call.active && s_state == SIP_PHONE_STATE_RINGING) {
                send_incoming_response(487, "Request Terminated", NULL);
            }
            stop_ring_signal();
            stop_rtp_rx();
            clear_incoming_call();
            s_state = SIP_PHONE_STATE_REGISTERED;
        } else if (sip_message_starts_with_method(message, "BYE")) {
            ESP_LOGI(TAG, "incoming SIP BYE");
            send_simple_response_for_message(sock, message, &remote_addr, remote_addr_len, 200, "OK");
            stop_ring_signal();
            stop_rtp_rx();
            clear_incoming_call();
            s_state = SIP_PHONE_STATE_REGISTERED;
        } else if (sip_message_starts_with_method(message, "ACK")) {
            ESP_LOGI(TAG, "incoming SIP ACK");
            if (s_state == SIP_PHONE_STATE_IN_CALL && s_incoming_call.active) {
                esp_err_t err = start_rtp_rx();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "start RTP RX after ACK failed: %s", esp_err_to_name(err));
                }
                err = start_rtp_tx();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "start RTP TX after ACK failed: %s", esp_err_to_name(err));
                }
                if (s_rtp_rx_running && s_rtp_tx_running) {
                    start_call_recording_from_dialog();
                }
            }
        } else if (sip_message_starts_with_method(message, "OPTIONS")) {
            ESP_LOGI(TAG, "incoming SIP OPTIONS");
            send_simple_response_for_message(sock, message, &remote_addr, remote_addr_len, 200, "OK");
        } else {
            ESP_LOGI(TAG, "incoming SIP request ignored: %.24s", message);
        }
    }
}

static esp_err_t ensure_incoming_sip_task(void)
{
    if (s_incoming_task != NULL) {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(incoming_sip_task,
                                     "sip_incoming",
                                     SIP_INCOMING_STACK_WORDS,
                                     NULL,
                                     5,
                                     &s_incoming_task);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "create incoming SIP task failed");
    return ESP_OK;
}

static esp_err_t wait_incoming_sip_socket_ready(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SIP_INCOMING_SOCKET_WAIT_MS);

    while (s_incoming_sock < 0) {
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

static void register_refresh_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SIP_REGISTER_REFRESH_SECONDS * 1000));

        if (!sip_phone_is_configured()) {
            continue;
        }

        for (int attempt = 1; attempt <= SIP_REGISTER_REFRESH_FAST_ATTEMPTS; ++attempt) {
            ESP_LOGI(TAG, "refreshing SIP registration, attempt %d/%d", attempt, SIP_REGISTER_REFRESH_FAST_ATTEMPTS);
            esp_err_t err = send_register();
            if (err == ESP_OK) {
                if (s_state == SIP_PHONE_STATE_REGISTERING || s_state == SIP_PHONE_STATE_ERROR) {
                    s_state = SIP_PHONE_STATE_REGISTERED;
                }
                ESP_LOGI(TAG,
                         "SIP registration refresh accepted, stack free=%u words",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
                break;
            }

            ESP_LOGW(TAG, "SIP registration refresh attempt %d failed: %s", attempt, esp_err_to_name(err));
            if (attempt == SIP_REGISTER_REFRESH_FAST_ATTEMPTS &&
                (s_state == SIP_PHONE_STATE_REGISTERED || s_state == SIP_PHONE_STATE_REGISTERING)) {
                s_state = SIP_PHONE_STATE_ERROR;
            }

            vTaskDelay(pdMS_TO_TICKS(SIP_REGISTER_RETRY_SECONDS * 1000));
        }
    }
}

static esp_err_t ensure_register_refresh_task(void)
{
    if (s_register_refresh_task != NULL) {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(register_refresh_task,
                                     "sip_reg_refresh",
                                     SIP_REGISTER_REFRESH_STACK_WORDS,
                                     NULL,
                                     4,
                                     &s_register_refresh_task);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "create SIP registration refresh task failed");
    ESP_LOGI(TAG, "SIP registration refresh task started every %d seconds", SIP_REGISTER_REFRESH_SECONDS);
    return ESP_OK;
}

esp_err_t sip_phone_start_registration(void)
{
    if (!sip_phone_is_configured()) {
        s_state = SIP_PHONE_STATE_REGISTERED;
        ESP_LOGW(TAG, "SIP server/user is empty; keeping registration in placeholder mode");
        return ESP_OK;
    }

    s_state = SIP_PHONE_STATE_REGISTERING;

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= SIP_REGISTER_START_ATTEMPTS; ++attempt) {
        ESP_LOGI(TAG, "SIP registration attempt %d/%d", attempt, SIP_REGISTER_START_ATTEMPTS);
        err = send_register();
        if (err == ESP_OK) {
            s_state = SIP_PHONE_STATE_REGISTERED;
            ESP_RETURN_ON_ERROR(ensure_incoming_sip_task(), TAG, "start incoming SIP listener failed");
            ESP_RETURN_ON_ERROR(wait_incoming_sip_socket_ready(), TAG, "incoming SIP listener socket not ready");
            return ensure_register_refresh_task();
        }

        ESP_LOGW(TAG, "SIP registration attempt %d failed: %s", attempt, esp_err_to_name(err));
        if (attempt < SIP_REGISTER_START_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(SIP_REGISTER_START_RETRY_SECONDS * 1000));
        }
    }

    s_state = SIP_PHONE_STATE_ERROR;
    return err;
}

esp_err_t sip_phone_answer(void)
{
    if (s_state == SIP_PHONE_STATE_RINGING) {
        stop_ring_signal();

        char sdp[512] = {0};
        esp_err_t err = build_basic_sdp(sdp, sizeof(sdp));
        if (err != ESP_OK) {
            return err;
        }

        err = send_incoming_response(200, "OK", sdp);
        if (err != ESP_OK) {
            return err;
        }

        s_state = SIP_PHONE_STATE_IN_CALL;
        ESP_LOGI(TAG, "answered incoming call, waiting for ACK to start RTP");
    } else {
        ESP_LOGI(TAG, "answer ignored in state %d", s_state);
    }
    return ESP_OK;
}

esp_err_t sip_phone_hangup(void)
{
    if (s_state == SIP_PHONE_STATE_CALLING) {
        s_outgoing_cancel_requested = true;
        stop_ringback();
        esp_err_t err = send_outgoing_cancel();
        s_state = SIP_PHONE_STATE_REGISTERED;
        ESP_LOGI(TAG, "cancel outgoing call");
        return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
    }

    if (s_state == SIP_PHONE_STATE_RINGING) {
        stop_ring_signal();
        esp_err_t err = send_incoming_response(486, "Busy Here", NULL);
        stop_rtp_rx();
        clear_incoming_call();
        s_state = SIP_PHONE_STATE_REGISTERED;
        ESP_LOGI(TAG, "rejected incoming call");
        return err;
    }

    if (s_state == SIP_PHONE_STATE_IN_CALL) {
        stop_ring_signal();
        esp_err_t err = send_in_call_bye();
        stop_rtp_rx();
        clear_incoming_call();
        s_state = SIP_PHONE_STATE_REGISTERED;
        ESP_LOGI(TAG, "hang up");
        return err;
    } else {
        ESP_LOGI(TAG, "hangup ignored in state %d", s_state);
    }
    return ESP_OK;
}

esp_err_t sip_phone_call_default_extension(void)
{
    if (s_state == SIP_PHONE_STATE_REGISTERED) {
        esp_err_t err = send_outgoing_invite();
        if (err != ESP_OK) {
            s_state = SIP_PHONE_STATE_REGISTERED;
            return err;
        }

        s_state = SIP_PHONE_STATE_IN_CALL;
        err = start_rtp_rx();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start outgoing RTP RX failed: %s", esp_err_to_name(err));
            stop_rtp_rx();
            clear_incoming_call();
            s_state = SIP_PHONE_STATE_REGISTERED;
            return err;
        }

        err = start_rtp_tx();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start outgoing RTP TX failed: %s", esp_err_to_name(err));
            stop_rtp_rx();
            clear_incoming_call();
            s_state = SIP_PHONE_STATE_REGISTERED;
            return err;
        }
        if (call_recorder_is_ready()) {
            esp_err_t rec_err = call_recorder_start(CONFIG_IPPHONE_SIP_USER, CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION);
            if (rec_err != ESP_OK) {
                ESP_LOGW(TAG, "call recording not started: %s", esp_err_to_name(rec_err));
            }
        }

        ESP_LOGI(TAG, "default outgoing call established with extension %s", CONFIG_IPPHONE_SIP_DEFAULT_EXTENSION);
    } else {
        ESP_LOGI(TAG, "call ignored in state %d", s_state);
    }
    return ESP_OK;
}
