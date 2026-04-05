#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>
#include <sys/un.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <libcamera/libcamera.h>
#include <libcamera/formats.h>
#include <libcamera/framebuffer_allocator.h>
#include <opencv2/opencv.hpp>

class SocketManager {
public:
    SocketManager(const char* socket_path) : path_(socket_path) {
        // remove existing socket file if it exists
        unlink(path_);

        // create server socket
        server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path_, sizeof(addr.sun_path) - 1);
        bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr));

        // listen and accept connection
        listen(server_fd_, 1);
        std::cout << "Waiting for cam_ai to connect to " << path_ << "..." << std::endl;
        client_fd_ = accept(server_fd_, NULL, NULL);
        std::cout << "cam_ai connected!" << std::endl;
    }
    void sendFrame(int idx, void* data, size_t size) {
        // send frame index followed by frame data
        send(client_fd_, &idx, sizeof(int), 0);
        send(client_fd_, data, size, 0);
    }
    bool receivePoints(int& idx, std::vector<cv::Point2f>& points) {
        // get frame index and 21 (x,y) points (42 float)
        if (recv(client_fd_, &idx, sizeof(int), 0) <= 0) return false;
        float coords[42];
        if (recv(client_fd_, coords, sizeof(coords), 0) <= 0) return false;
        points.clear();
        for (int i = 0; i < 21; ++i) points.push_back(cv::Point2f(coords[i*2], coords[i*2+1]));
        return true;
    }
private:
    int server_fd_, client_fd_;
    const char* path_;
};

struct FrameData {
    libcamera::Request *request;
    void *mapped_ptr;
    size_t length;
};

static const std::vector<std::pair<int, int>> HAND_CONNECTIONS = {
    {0, 1}, {1, 2}, {2, 3}, {3, 4},         // 大拇指
    {0, 5}, {5, 6}, {6, 7}, {7, 8},         // 食指
    {0, 9}, {9, 10}, {10, 11}, {11, 12},    // 中指
    {0, 13}, {13, 14}, {14, 15}, {15, 16},  // 無名指
    {0, 17}, {17, 18}, {18, 19}, {19, 20},  // 小拇指
    {5, 9}, {9, 13}, {13, 17}               // 掌心
};

class FrameSyncManager {
public:
    FrameSyncManager(libcamera::Camera *cam, uint32_t w, uint32_t h, uint32_t s)
        : camera_(cam), width_(w), height_(h), stride_(s), 
          last_time_(std::chrono::steady_clock::now()), fps_(0.0) {}
    void pushFrame(int idx, libcamera::Request *req, libcamera::Stream *main_stream) {
        std::lock_guard<std::mutex> lock(mtx_); // auto destroy lock
        libcamera::FrameBuffer *buffer = req->buffers().at(main_stream);
        int fd = buffer->planes()[0].fd.get();
        size_t len = buffer->planes()[0].length;
        void *ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        buffer_map_[idx] = {req, ptr, len};
        
        if (buffer_map_.size() > 10) { // discard old frames if buffer exceeds 10
            auto it = buffer_map_.begin();
            releaseFrame(it->second);
            buffer_map_.erase(it);
        }
    }
    void updateAndShow(int idx, const std::vector<cv::Point2f>& points) {
        std::lock_guard<std::mutex> lock(mtx_); // auto destroy lock
        if (buffer_map_.find(idx) == buffer_map_.end()) return;

        // calculate FPS
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - last_time_;
        last_time_ = now;
        double current_fps = 1.0 / elapsed.count();
        fps_ = (fps_ * 0.9) + (current_fps * 0.1);

        FrameData &data = buffer_map_[idx];

        // image processing
        cv::Mat y_plane(height_, width_, CV_8UC1, data.mapped_ptr, stride_); // Y-only Mat (Zero-copy)
        cv::Mat rgb_frame;
        cv::cvtColor(y_plane, rgb_frame, cv::COLOR_GRAY2BGR);

        if (points.size() == 21) {
            for (const auto& conn : HAND_CONNECTIONS) {
                cv::Point p1(points[conn.first].x * width_, points[conn.first].y * height_);
                cv::Point p2(points[conn.second].x * width_, points[conn.second].y * height_);
                cv::line(rgb_frame, p1, p2, cv::Scalar(255, 255, 0), 3, cv::LINE_AA);
            }
            for (const auto& p : points) {
                cv::Point center(p.x * width_, p.y * height_);
                cv::circle(rgb_frame, center, 8, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
            }
        }

        // draw FPS and buffer count
        std::string fps_text = "FPS: " + std::to_string((int)std::round(fps_));
        cv::putText(rgb_frame, fps_text, cv::Point(50, 80), 
                    cv::FONT_HERSHEY_SIMPLEX, 2.0, cv::Scalar(0, 255, 0), 3);
        
        std::string buf_text = "Buffer: " + std::to_string(buffer_map_.size());
        cv::putText(rgb_frame, buf_text, cv::Point(50, 150), 
                    cv::FONT_HERSHEY_SIMPLEX, 1.5, cv::Scalar(0, 200, 255), 2);

        // display and cleanup
        cv::imshow("Master Preview (RGB Mode)", rgb_frame);
        cv::waitKey(1);

        releaseFrame(data);
        buffer_map_.erase(idx);
    }
private:
    void releaseFrame(FrameData &data) {
        munmap(data.mapped_ptr, data.length);
        data.request->reuse(libcamera::Request::ReuseBuffers);
        camera_->queueRequest(data.request);
    }
    libcamera::Camera *camera_;
    uint32_t width_, height_, stride_;
    std::map<int, FrameData> buffer_map_;
    std::mutex mtx_;
    std::chrono::steady_clock::time_point last_time_;
    double fps_;
};

class CameraHandler {
public:
    CameraHandler() : cm_(std::make_unique<libcamera::CameraManager>()) { cm_->start(); }
    ~CameraHandler() { 
        stop(); 
        if (camera_) camera_->release(); 
        cm_->stop(); 
    }
    bool init(int main_w, int main_h, int ai_w, int ai_h) {
        if (cm_->cameras().empty()) return false;
        // Select the first available camera and acquire exclusive control
        camera_ = cm_->cameras()[0];
        camera_->acquire();
        config_ = camera_->generateConfiguration({ libcamera::StreamRole::VideoRecording, libcamera::StreamRole::Viewfinder });
        // Configure Main Stream (High Res)
        config_->at(0).size = { (unsigned int)main_w, (unsigned int)main_h };
        config_->at(0).pixelFormat = libcamera::formats::YUV420;
        // Configure AI Stream (Low Res)
        config_->at(1).size = { (unsigned int)ai_w, (unsigned int)ai_h };
        config_->at(1).pixelFormat = libcamera::formats::YUV420;
        // Validate ensures the hardware can actually support these settings
        config_->validate();
        camera_->configure(config_.get());
        // Allocate memory buffers for each configured stream
        allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);
        for (auto &cfg : *config_) allocator_->allocate(cfg.stream());
        return true;
    }
    void start(void (*callback)(libcamera::Request *)) {
        libcamera::Stream *main_s = const_cast<libcamera::Stream *>(config_->at(0).stream());
        libcamera::Stream *ai_s = const_cast<libcamera::Stream *>(config_->at(1).stream());
        const auto &main_bufs = allocator_->buffers(main_s);
        const auto &ai_bufs = allocator_->buffers(ai_s);
        // Map multiple buffers into Requests to keep the hardware pipeline full
        for (size_t i = 0; i < main_bufs.size(); ++i) {
            std::unique_ptr<libcamera::Request> request = camera_->createRequest();
            request->addBuffer(main_s, main_bufs[i].get());
            request->addBuffer(ai_s, ai_bufs[i].get());
            requests_.push_back(std::move(request));
        }
        // Connect the signal handler and start the camera hardware
        camera_->requestCompleted.connect(callback);
        camera_->start();
        // Queue all prepared requests to the hardware
        for (auto &request : requests_) camera_->queueRequest(request.get());
    }
    void stop() { 
        if (camera_) camera_->stop();
        requests_.clear();
    }
    libcamera::Stream* getMainStream() { return config_->at(0).stream(); }
    libcamera::Stream* getAIStream() { return config_->at(1).stream(); }
    libcamera::Camera* getCamera() { return camera_.get(); }
    unsigned int getMainStride() { return config_->at(0).stride; }
private:
    std::unique_ptr<libcamera::CameraManager> cm_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> config_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;
};

// global variables
CameraHandler* g_handler = nullptr;
FrameSyncManager* g_sync = nullptr;
SocketManager* g_socket = nullptr;
int g_frame_counter = 0;

void socketReceiverThread() {
    while (true) {
        int idx;
        std::vector<cv::Point2f> points;
        if (g_socket && g_socket->receivePoints(idx, points)) {
            g_sync->updateAndShow(idx, points);
        }
    }
}

void onRequestComplete(libcamera::Request *request) {
    if (request->status() == libcamera::Request::RequestCancelled) return;
    int current_idx = g_frame_counter++;
    
    // send main stream frame to FrameSyncManager for processing and display
    g_sync->pushFrame(current_idx, request, g_handler->getMainStream());

    // send AI stream frame to Python via Socket (Zero-copy)
    libcamera::Stream *ai_s = g_handler->getAIStream();
    libcamera::FrameBuffer *buffer = request->buffers().at(ai_s);
    void *ptr = mmap(NULL, buffer->planes()[0].length, PROT_READ, MAP_SHARED, buffer->planes()[0].fd.get(), 0);
    
    if (g_socket) g_socket->sendFrame(current_idx, ptr, 256 * 256);
    
    munmap(ptr, buffer->planes()[0].length);
}

int main() {
    // init camera, sync manager, and socket
    g_handler = new CameraHandler();
    if (!g_handler->init(1232, 1232, 256, 256)) return -1;

    g_sync = new FrameSyncManager(g_handler->getCamera(), 1232, 1232, g_handler->getMainStride());
   
    g_socket = new SocketManager("/tmp/cam.sock");

    // start socket receiver thread
    std::thread receiver(socketReceiverThread);
    receiver.detach();

    // activate camera and start processing frames
    g_handler->start(onRequestComplete);

    while (true) { std::this_thread::sleep_for(std::chrono::seconds(1)); }
    return 0;
}