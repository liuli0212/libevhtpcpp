#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "common/string/strutil.h"
#include "glog/logging.h"
#include "libevent/event2/buffer.h"
#include "http_request.h"

namespace {
struct evbuffer* BuildBuffer(const std::string& data) {
    struct evbuffer* buf = NULL;
    if (data.size() > 0) {
        buf = evbuffer_new();
        evbuffer_add(buf, data.data(), data.size());
    }
    return buf;
}

int kv_cb(evhtp_kv_t* kv, void* arg) {
    std::string* str = static_cast<std::string*>(arg);
    char buf[1024];
    snprintf(buf, sizeof(buf), "    '%s: %s'\n", kv->key, kv->val);
    str->append(buf);
    return 0;
}

std::string EventErrorToString(evhtp_error_flags event) {
    if (event == (evhtp_error_flags)-1)
        return "DataTooLong";

    struct Events {
        evhtp_error_flags event;
        const char* event_str;
    } kEvents[] = {
        { BEV_EVENT_READING, "BEV_EVENT_READING" },
        { BEV_EVENT_WRITING, "BEV_EVENT_WRITING"},
        { BEV_EVENT_EOF, "BEV_EVENT_EOF" },
        { BEV_EVENT_ERROR, "BEV_EVENT_ERROR" },
        { BEV_EVENT_TIMEOUT, "BEV_EVENT_TIMEOUT" },
        { BEV_EVENT_CONNECTED, "BEV_EVENT_CONNECTED" },
        {0, NULL},
    };
    std::string ret;
    for (int i = 0; kEvents[i].event_str != NULL; ++i) {
        if (event & kEvents[i].event) {
            if (ret.size())
                ret.append("|");
            ret.append(kEvents[i].event_str);
        }
    }
    return ret;
}

}  // namespace

HttpRequest::HttpRequest(evhtp_request_t* req)
    : req_(req), done_(false), write_cb_(NULL) {
}

HttpRequest::~HttpRequest() {
    if (write_cb_) {
        delete write_cb_;
    }
}

// TODO: It copies data twice, can be optimed to return raw pointer.
std::string HttpRequest::GetBody(size_t max_size) const {
    std::string ret;
    size_t len = evbuffer_get_length(req_->buffer_in);
    if (len == 0)
        return ret;
    if (max_size > 0 && len > max_size)
        len = max_size;
    char* buf = new char[len];
    evbuffer_copyout(req_->buffer_in, buf, len);
    ret.reserve(len);
    ret.assign(buf, len);
    delete[] buf;
    return ret;
}

std::string HttpRequest::DumpRequest(bool need_body) const {
    std::string buf;
    common::StringPrintfAppend(&buf, "Path: %s\n", req_->uri->path->full);
    if (req_->uri->fragment) {
        common::StringPrintfAppend(
                &buf, "Fragment: %s\n", req_->uri->fragment);
    }
    if (req_->uri->query) {
        std::string q;
        evhtp_kvs_for_each(req_->uri->query, kv_cb, (void*)&q);
        buf.append("Query: \n");
        buf.append(q);
    }
    if (req_->headers_in) {
        std::string q;
        evhtp_kvs_for_each(req_->headers_in, kv_cb, (void*)&q);
        buf.append("Headers: \n");
        buf.append(q);
    }

    if (need_body) {
        buf.append("\n-- START BODY --\n");
        buf.append(GetBody(1024));
        buf.append("\n-- END BODY --\n");
    }
    return buf;
}

void HttpRequest::AddHeader(const std::string& key, const std::string& value) {
    evhtp_headers_add_header(req_->headers_out,
            evhtp_header_new(key.c_str(), value.c_str(), 1, 1));
}

const char* HttpRequest::GetQuery(const char* key) const {
    if (!req_->uri || !req_->uri->query)
        return NULL;
    return evhtp_kv_find(req_->uri->query, key);
}

const char* HttpRequest::GetHeader(const char* key) const {
    if (!req_->headers_in)
        return NULL;
    return evhtp_kv_find(req_->headers_in, key);
}

bool HttpRequest::Reply(const std::string& data, int response_code) {
    if (req_->error) {
        return false;
    }

    evbuffer_add(req_->buffer_out, data.data(), data.size());
    evhtp_send_reply(req_, response_code);
    done_ = true;
    return req_->error == 0;
}

bool HttpRequest::StartChunkReply(int response_code) {
    if (req_->error)
        return false;

    evhtp_send_reply_chunk_start(req_, response_code);
    return req_->error == 0;
}

bool HttpRequest::SendChunk(const std::string& data) {
    if (req_->error)
        return false;

    struct evbuffer* buf = BuildBuffer(data);
    if (buf != NULL) {
        evhtp_send_reply_chunk(req_, buf);
        evbuffer_free(buf);
    }
    return req_->error == 0;
}

bool HttpRequest::SendChunk(const char* data, int data_len) {
    if (req_->error || data_len <= 0 || data == NULL)
        return false;

    struct evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, data, data_len);
    evhtp_send_reply_chunk(req_, buf);
    evbuffer_free(buf);
    return req_->error == 0;
}

size_t HttpRequest::PendingDataSize() const {
    return evbuffer_get_length(bufferevent_get_output(req_->conn->bev));
}

bool HttpRequest::EndChunkReply() {
    if (req_->error)
        return false;

    done_ = true;
    evhtp_send_reply_chunk_end(req_);
    return true;
}

void HttpRequest::EndRequest() {
    done_ = true;

    // Mark request finished. The tricky part is we won't free the request or
    // the underlying connection, because it may results invalid memory access,
    // e.g. when this function invoked in the on_write hook, libevhtp may try
    // to access the request after calling the on_write hook, thus results in
    // core dump.
    //
    // Just mark it finished, the request will be properly freed when all the
    // buffered data are transported in the end of the write cb.
    req_->finished = 1;
}

evhtp_res HttpRequest::OnWrite(evhtp_connection_t* conn, void* arg) {
    HttpRequest* _this = (HttpRequest*)arg;
    CHECK(conn == _this->req_->conn);
    if (!_this->done_ && _this->write_cb_ != NULL) {
        if (!_this->write_cb_->Run(_this)) {
            // Something bad happend during a chunk write. Close the connection
            // to signal an error.
            _this->EndRequest();
        }
    }
    return EVHTP_RES_OK;
}

evhtp_res HttpRequest::OnFinish(evhtp_request_t* req, void* arg) {
    HttpRequest* _this = (HttpRequest*)arg;
    evhtp_unset_all_hooks(&req->conn->hooks);
    if (_this->write_cb_ != NULL) {
        // Use NULL means finishing the request.
        _this->write_cb_->Run(NULL);
    }
    req->priv = NULL;
    delete _this;
    return EVHTP_RES_OK;
}

evhtp_res HttpRequest::OnError(
        evhtp_request_t* req, evhtp_error_flags flags, void* arg) {
    LOG(ERROR) << common::StringPrintf(
            "Error connection: %s, code: %s (0x%X)",
            HttpRequest::RequestToShortString(req).c_str(),
            EventErrorToString(flags).c_str(), flags);

    return EVHTP_RES_OK;
}

void HttpRequest::SetWriteCallback(WriteCallback* cb) {
    if (write_cb_)
        delete write_cb_;
    write_cb_ = cb;
}

std::string HttpRequest::RequestToShortString(evhtp_request_t* req) {
    const char* path = NULL;
    if (req->uri && req->uri->path) {
        path = req->uri->path->full;
    }
    char* addr = NULL;
    if (req->conn && req->conn->saddr) {
        addr = inet_ntoa(((struct sockaddr_in*)req->conn->saddr)->sin_addr);
    }
    unsigned char* query = NULL;
    if (req->uri)
        query = req->uri->query_raw;
    return common::StringPrintf("%s?%s\t%s", path, query, addr);
}

std::string HttpRequest::RequestToShortString() const {
    return HttpRequest::RequestToShortString(req_);
}
