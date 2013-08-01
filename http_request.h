#ifndef HTTP_REQUEST_H_
#define HTTP_REQUEST_H_

#include <map>
#include <string>
#include "common/base/closure.h"
#include "evhtp.h"

class HttpRequest {
    public:
        explicit HttpRequest(evhtp_request_t* req);
        ~HttpRequest();

        std::string GetBody(size_t max_size) const;

        void AddHeader(const std::string& key, const std::string& value);

        // Return value string of key from the parsed query.
        // Return NULL if not found.
        const char* GetQuery(const char* key) const;

        // Return value of specified header.
        // Return NULL if not found.
        const char* GetHeader(const char* key) const;

        bool Reply(const std::string& data, int response_code);

        typedef Closure<bool, HttpRequest*> WriteCallback;
        // HttpRequest owns the callback.
        // Normally will run the callback with the request associated with it,
        // if calling with NULL, the callback can do some cleanup to finish
        // the request.
        void SetWriteCallback(WriteCallback* cb);

        // Get the size of data that is in buffer.
        size_t PendingDataSize() const;

        // Chunked reply.
        bool StartChunkReply(int response_code);
        // Send a data chunk.
        // In case of the underlying buffer is full, will loop until it has
        // space for error happened.
        bool SendChunk(const std::string& data);
        bool SendChunk(const char* data, int data_len);
        bool EndChunkReply();

        // End and free the request.
        // Only used to close a connection immediately.
        void EndRequest();

        // Called when there is spare room in the buffer, can send more data.
        static evhtp_res OnWrite(evhtp_connection_t* conn, void* arg);
        // Called when the request is finishing.
        static evhtp_res OnFinish(evhtp_request_t* req, void* arg);
        // Called when there is any error in the request.
        static evhtp_res OnError(
                evhtp_request_t* req, evhtp_error_flags flags, void* arg);

        // Return a human readable request dump.
        std::string DumpRequest(bool need_body) const;
        static std::string RequestToShortString(evhtp_request_t* req);
        std::string RequestToShortString() const;

    private:
        evhtp_request_t* req_;
        bool done_;
        WriteCallback *write_cb_;
};
