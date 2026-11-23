#include "detect/inference_capture.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

std::string saveInferenceCapture(const CameraImages& frames, const CameraTransforms& transforms,
                                 const std::filesystem::path& directory, uint32_t sequence,
                                 bool missedDart, bool& ok)
{
    namespace fs = std::filesystem;
    static std::mutex writer;
    std::lock_guard<std::mutex> lock(writer);
    ok = false;
    std::vector<fs::path> created;
    try
    {
        fs::create_directories(directory);
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm,&time);
#else
        localtime_r(&time,&tm);
#endif
        std::ostringstream stamp;
        stamp << std::put_time(&tm,"%Y%m%d-%H%M%S-") << std::setw(3) << std::setfill('0')
              << std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()%1000;
        std::string stem=stamp.str();
        for(unsigned suffix=1; fs::exists(directory/(stem+".json")); ++suffix)
            stem=stamp.str()+"-"+std::to_string(suffix);
        std::ostringstream json;
        json << std::setprecision(17) << "{\n  \"stem\": \"" << stem
             << "\",\n  \"canonical_size\": " << WARPED_OUTPUT_SIZE
             << ",\n  \"frame_sequence\": " << sequence;
        if(missedDart) json << ",\n  \"missed_dart\": true";
        json << ",\n  \"cameras\": {";
        bool first=true;
        for(size_t i=0; i<frames.size(); ++i)
        {
            if(frames[i].empty()) continue;
            const auto filename=stem+"_cam"+std::to_string(i)+".png";
            cv::Mat bgr;
            cv::cvtColor(frames[i],bgr,cv::COLOR_RGB2BGR);
            created.push_back(directory/filename);
            if(!cv::imwrite(created.back().string(),bgr))
                throw std::runtime_error("could not write "+filename);
            const auto& c=transforms[i];
            if(!c.calibrated) continue;
            json << (first ? "\n" : ",\n"); first=false;
            json << "    \"" << i << "\": {\"image\": \"" << filename << "\", \"width\": "
                 << frames[i].cols << ", \"height\": " << frames[i].rows << ", \"homography\": [";
            for(size_t j=0;j<c.homography.size();++j) json << (j?", ":"") << c.homography[j];
            json << "], \"wire_points\": [";
            for(size_t j=0;j<c.points.size();++j) json << (j?", ":"") << "[" << c.points[j].x << ", " << c.points[j].y << "]";
            json << "]}";
        }
        if(created.empty()) return "no usable camera frames";
        created.push_back(directory/(stem+".json.tmp"));
        std::ofstream out(created.back(),std::ios::binary);
        out << json.str() << "\n  }\n}\n";
        out.close();
        if(!out) throw std::runtime_error("could not write calibration sidecar");
        fs::rename(created.back(),directory/(stem+".json"));
        ok=true;
        return (directory/stem).string();
    }
    catch(const std::exception& e)
    {
        std::error_code ignored;
        for(const auto& path:created) fs::remove(path,ignored);
        return e.what();
    }
}
