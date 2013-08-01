#ifndef DISPATCHER_H_
#define DISPATCHER_H_

#include <map>
#include <string>

#include "common/base/closure.h"
#include "libevhtp/evhtp.h"

class HttpRequest;

class Dispatcher {
    typedef Closure<void, HttpRequest*> Callback;
    public:
        // Register a handler that will be invoked when 'cmd' is received.
        bool Register(const std::string& cmd, Callback* callback);

        // Start the server on *port*.
        void Start(int port);

    private:
        static void RequestHandlerEntry(evhtp_request_t* req, void* arg);
        void HandleRequest(evhtp_request_t* req);

        // Hook libevhtp to setup request before and after it is accepted.
        static evhtp_res OnRequestAccepted(evhtp_connection_t * conn, void * arg);
        static evhtp_res OnRequestPreAccepted(
                evhtp_connection_t * conn, void * arg);
        static evhtp_res OnHeadersParsed(evhtp_request_t* req, void * arg);

        bool RunCallback(const std::string& cmd,
                evhtp_request_t* req);

        std::map<std::string, Callback*> callbacks_;
};

#endif  // DISPATCHER_H_
