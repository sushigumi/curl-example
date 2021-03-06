#include "HttpService.h"
#include <memory>



#pragma comment(lib, "Ws2_32.lib")


HttpService& HttpService::GetInstance() {
    static HttpService httpService;
    return httpService;
}


HttpService::HttpService()  {
    // Global init
    curl_global_init(CURL_GLOBAL_ALL);

    // Set variables
    m_pMultiHandle = curl_multi_init();
    m_StillRunning = 0;
    m_Run = 1;

    curl_multi_setopt(m_pMultiHandle, CURLMOPT_MAXCONNECTS, (long)10);

    m_EventThread = std::thread(&HttpService::EventLoop, this);
}

void HttpService::Cleanup() {
    m_Run = 0;

    // Wait for the thread to finish execution
    m_EventThread.join();


    curl_multi_cleanup(m_pMultiHandle);
}

std::future<std::string> HttpService::GetAsync(std::string url) {
    std::promise<std::string> promise;
    auto httpRequest = std::make_shared<HttpRequestFuture>(std::move(promise));
    SetupRequest(HttpMethodType::GET, url, httpRequest, "");

    return httpRequest->promise.get_future();
}

void HttpService::GetAsync(std::string url, std::function<void(std::string)> callback) {
    auto httpRequest = std::make_shared<HttpRequestCallback>(std::move(callback));
    SetupRequest(HttpMethodType::GET, url, httpRequest, "");
}

std::future<std::string> HttpService::PostAsync(std::string url, std::string body) {
    std::promise<std::string> promise;
    auto httpRequest = std::make_shared<HttpRequestFuture>(std::move(promise));
    SetupRequest(HttpMethodType::POST, url, httpRequest, body);

    return httpRequest->promise.get_future();
}

void HttpService::SetupRequest(HttpMethodType method, std::string url, std::shared_ptr<HttpRequest> httpRequest, std::string body) {
    CURL* handle = curl_easy_init();

    // Set options
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());

    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &HttpService::WriteData); //working
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &(httpRequest->str));

    // Set up the POST fields for a POST request
    httpRequest->body = body;
    if (method == HttpMethodType::POST) {
        struct curl_slist* chunk = NULL;
        chunk = curl_slist_append(chunk, "Content-Type: application/json");

        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, chunk);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, httpRequest->body.c_str());
    }

    httpRequest->handle = handle;

    // Add the handle to the multi stack
    curl_multi_add_handle(m_pMultiHandle, handle);

    // Perform the action
    curl_multi_perform(m_pMultiHandle, &m_StillRunning);

    m_Callbacks.push_back(httpRequest);
    m_Transfers++;
}

void HttpService::FinishRequest(CURL* handle) {
    curl_easy_cleanup(handle);
}

void HttpService::EventLoop() {
    CURLMsg* msg;
    int msgs_left = -1;

    while (m_Run || m_Transfers) {
        // Get which messages are done
        while ((msg = curl_multi_info_read(m_pMultiHandle, &msgs_left))) {
            if (msg->msg == CURLMSG_DONE) {
                char* url;
                CURL* e = msg->easy_handle;
                curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &url);
                printf("R: %d - %s <%s>\n", msg->data.result, curl_easy_strerror(msg->data.result), url);

                for (auto it = m_Callbacks.begin(); it != m_Callbacks.end();) {
                    if ((*it)->handle == e) {

                        (*it)->Callback();
                        
                        // Remove this since completed
                        m_Callbacks.erase(it);
                        m_Transfers--;
                        break;
                    }
                    else {
                        ++it;
                    }
                }
                // Cleanup after calling the callback (might move this to destructor? 
                // but maybe not...)
                curl_multi_remove_handle(m_pMultiHandle, e);
                curl_easy_cleanup(e);
            }
        }

        printf("Running event loop: %d running\n", m_StillRunning);
        struct timeval timeout;
        int rc; // select() return code
        CURLMcode mc; // curl_multi_fdset return code

        fd_set fdread;
        fd_set fdwrite;
        fd_set fdexcep;
        int maxfd = -1;

        long curl_timeo = -1;

        FD_ZERO(&fdread);
        FD_ZERO(&fdwrite);
        FD_ZERO(&fdexcep);

        // set a suitable timeout
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        curl_multi_timeout(m_pMultiHandle, &curl_timeo);
        if (curl_timeo >= 0) {
            timeout.tv_sec = curl_timeo / 1000;
            if (timeout.tv_sec > 1)
                timeout.tv_sec = 1;
            else
                timeout.tv_usec = (curl_timeo % 1000) * 1000;
        }

        // Get the file descriptors from the transfers
        mc = curl_multi_fdset(m_pMultiHandle, &fdread, &fdwrite, &fdexcep, &maxfd);

        if (mc != CURLM_OK) {
            printf("curl_multi_fdset() failed, code %d.\n", mc);
            break;
        }

        // On success the value of maxfd is guaranteed to be >= -1. We call
        // select(maxfd + 1, ...); specially in the case of (maxfd == -1) 
        // there are no fds ready yet so we call select(0, ...) --or Sleep() 
        // on Windows-- to sleep 100ms, which is the minimum suggested value 
        // in curl_multi_fdset() doc.
        if (maxfd == -1) {
            printf("In maxfd -1\n");
            Sleep(100);
            rc = 0;
        }
        else {
            printf("In maxfd != -1\n");
            rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);
        }

        printf("RC is %d\n", rc);
        switch (rc) {
        case -1:
            break;
        case 0:
        default:
            // Timeout or readable/writable sockets
            curl_multi_perform(m_pMultiHandle, &m_StillRunning);
            break;
        }
    }
       
}

size_t HttpService::WriteData(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t realsize = size * nmemb;

    try {
        userp->append((char*)contents, realsize);
    }
    catch (std::bad_alloc& e) {
        printf("asdf\n");
    }

    return realsize;


    /*if (writerData == NULL)
        return 0;

    writerData->append(data, size * nmemb);

    return size * nmemb;*/
}
