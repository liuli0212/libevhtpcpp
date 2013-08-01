#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "gflags/gflags.h"
#include "glog/logging.h"
#include "libevent/event2/event.h"
#include "libevent/event2/buffer.h"
#include "libevent/event2/thread.h"
#include "miuicloud/storage/miuistorage/http_request.h"
#include "miuicloud/storage/miuistorage/dispatcher.h"

DEFINE_int32(http_threads, 17, "Number of threads for http server.");

using std::map;
using std::string;

namespace {

void SetTimeOut(int socket, int read_timeout_ms, int write_timeout_ms) {
    if (socket <= 0) {
        LOG(ERROR) << "Invalid socket";
        return;
    }


    struct timeval t;
    if (read_timeout_ms > 0) {
        t.tv_sec = (int)(read_timeout_ms / 1000);
        t.tv_usec = (int)((read_timeout_ms % 1000)*1000);
        int ret = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                (char*)(&t), sizeof(t));
        if (ret == -1) {
            LOG(ERROR) << "Failed to set recv timeout.";
        }
    }
    if (write_timeout_ms > 0) {
        t.tv_sec = (int)(write_timeout_ms / 1000);
        t.tv_usec = (int)((write_timeout_ms % 1000)*1000);
        int ret = setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                (char*)(&t), sizeof(t));
        if (ret == -1) {
            LOG(ERROR) << "Failed to set send timeout.";
        }
    }
}
} // namespace

bool Dispatcher::Register(const string& cmd, Callback* callback) {
    if (callbacks_.count(cmd) == 0) {
        callbacks_[cmd] = callback;
        return true;
    }
    LOG(ERROR) << "Already registered callback for command: " << cmd;
    return false;
}

bool Dispatcher::RunCallback(const std::string& cmd, evhtp_request_t* req) {
   if (req->priv == NULL) {
     LOG(ERROR) << "Haven't create HttpRequest?";
     return false;
   }

    if (callbacks_.count(cmd) > 0) {
        callbacks_[cmd]->Run(static_cast<HttpRequest*>(req->priv));
        return true;
    } else {
        return false;
    }
}

void Dispatcher::RequestHandlerEntry(
        evhtp_request_t *req, void *arg) {
    static_cast<Dispatcher*>(arg)->HandleRequest(req);
}

void Dispatcher::HandleRequest(evhtp_request_t* req) {
    if (!req || !req->uri || !req->uri->path || !req->uri->path->full) {
        LOG(ERROR) << "Invalid uri.";
        evhtp_send_reply(req, EVHTP_RES_NOTFOUND);
        return;
    }
    if (!RunCallback(req->uri->path->full, req)) {
        LOG(ERROR) << "No callbacks registered.";
        evhtp_send_reply(req, EVHTP_RES_NOTFOUND);
    }
    // Now, Registered handlers are responsible for replying req.
}

evhtp_res Dispatcher::OnRequestPreAccepted(evhtp_connection_t* conn, void * arg) {
    // Read/write to client timeout defaults to 60s (take from nginx).
    // NOTE: seems not working...
    SetTimeOut(conn->sock, 60000, 60000);
    return EVHTP_RES_OK;
}

evhtp_res Dispatcher::OnRequestAccepted(evhtp_connection_t* conn, void * arg) {
    static const int kMaxBodySize = 8 * 1024;  // 8k.
    evhtp_connection_set_max_body_size(conn, kMaxBodySize);

    evhtp_set_hook(&conn->hooks, evhtp_hook_on_headers,
            (evhtp_hook)Dispatcher::OnHeadersParsed, arg);
    return EVHTP_RES_OK;
}

evhtp_res Dispatcher::OnHeadersParsed(evhtp_request_t* evreq, void * arg) {
    HttpRequest* req = new HttpRequest(evreq);
    LOG(INFO) << req->RequestToShortString();

    evhtp_set_hook(&evreq->conn->hooks, evhtp_hook_on_write,
            (evhtp_hook)HttpRequest::OnWrite, req);
    evhtp_set_hook(&evreq->hooks, evhtp_hook_on_request_fini,
            (evhtp_hook)HttpRequest::OnFinish, req);
    evhtp_set_hook(&evreq->hooks, evhtp_hook_on_error,
            (evhtp_hook)HttpRequest::OnError, req);

    // Cache the request.
    evreq->priv = (void*)req;
    return EVHTP_RES_OK;
}

void Dispatcher::Start(int port) {
    evthread_use_pthreads();

    struct event_base *base = event_base_new();
	if (!base) {
		LOG(ERROR) << "Failed to create event base.";
		return;
	}

	evhtp_t* http = evhtp_new(base, NULL);
	if (!http) {
		LOG(ERROR) << "Failed to create http.";
		return;
	}
    evhtp_set_bev_flags(http, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
	evhtp_set_gencb(http, RequestHandlerEntry, this);

    // Start the worker threads.
    evhtp_use_threads(http, NULL, FLAGS_http_threads, NULL);
    evhtp_set_max_keepalive_requests(http, -1);

    // Hook that will be invoked before a request is accepted.
    evhtp_set_pre_accept_cb(http, Dispatcher::OnRequestPreAccepted, this);

    // Hook that will be invoked after a request is accepted.
    evhtp_set_post_accept_cb(http, Dispatcher::OnRequestAccepted, this);

    int handle = evhtp_bind_socket(http, "0.0.0.0", port, 512);
	if (handle != 0) {
        LOG(ERROR) << "Couldn't bind to port: " << port;
        // evhtp_free(http);
		return;
	}
    LOG(INFO) << "Server listening at port: " << port;
    event_base_dispatch(base);

    // evhtp_free(http);
}
