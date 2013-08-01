// A demo showing how to use the dispatcher.
#include <algorithm>
#include <map>
#include <string>
#include <stdlib.h>
#include <vector>

#include "common/string/strutil.h"
#include "common/thread/thread.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "dispatcher.h"
#include "http_request.h"

using std::map;
using std::string;

namespace {

class Writer {
    public:
        explicit Writer(size_t sz) : wrote_(0), size_(sz) {
        }

        // Called when the socket is writable, and there is spare room
        // in the buffer.
        // Will write a chunk of data.
        bool WriteChunk(miuistorage::HttpRequest* req) {
            if (req == NULL) {
                // The request is finishing.
                LOG(INFO) << "Finishing.";
                delete this;
                return true;
            }

            size_t to_write = std::min((size_t)1024000, size_ - wrote_);
            string msg;
            if (to_write > 0)
                msg.reserve(to_write);
            for (size_t j = 0; j < to_write; ++j) {
                msg.append("C");
            }

            if (wrote_ == 0) {
                if (!req->StartChunkReply(200)) {
                    LOG(ERROR) << "Failed to start chunk write.";
                    return false;
                }
                if (!req->SendChunk(msg)) {
                    LOG(ERROR) << "Failed to write first chunk.";
                    return false;
                }
                wrote_ += msg.size();
                return true;
            } else if (wrote_ < size_) {
                static const size_t kMaxPending = 10*1000*1000;
                // Avoid too much pending data in buffer.
                if (req->PendingDataSize() > kMaxPending) {
                    LOG(INFO) << "Too big buffer.";
                    return false;
                }
                if (!req->SendChunk(msg)) {
                    LOG(ERROR) << "Failed to write data";
                    return false;
                }
                wrote_ += msg.size();
                return true;
            } else {
                LOG(INFO) << "End chunk.";
                if (!req->EndChunkReply()) {
                    // TODO(liuli): what should we do?
                    LOG(ERROR) << "Failed REPLY to free data";
                    return false;
                }
            }
            return false;
        }

private:
    size_t wrote_;
    size_t size_;
};

void HelloHandler(miuistorage::HttpRequest* req) {
    LOG(INFO) << "HelloHandler";
    Writer* w = new Writer(10*1000*1000);
    req->SetWriteCallback(NewPermanentClosure(w, &Writer::WriteChunk));
    // Should write something to the connection in order to enable write.
    if (!w->WriteChunk(req))
        req->EndRequest();
    return;
}

void WorldHandler(miuistorage::HttpRequest* req) {
    LOG(INFO) << "World";
    LOG(INFO) << req->DumpRequest(true);
    req->AddHeader("Content-Type", "application/x-json");
    req->Reply("World\n", 300);
    //HeapProfilerDump("check");
    return;
}

void StopHandler(miuistorage::HttpRequest* req) {
    LOG(INFO) << "stop";
    req->AddHeader("Content-Type", "application/x-json");
    req->Reply("World", 200);
    //HeapProfilerDump("done");
    exit(0);
}

}  // namespace

int main(int argc, char **argv) {
    //HeapProfilerStart("/tmp/profiler");
    google::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();

    miuistorage::Dispatcher dispatcher;

    dispatcher.Register("/hello", NewPermanentClosure(HelloHandler));
    dispatcher.Register("/world", NewPermanentClosure(WorldHandler));
    dispatcher.Register("/stop", NewPermanentClosure(StopHandler));

    dispatcher.Start(8889);
    return 0;
}
