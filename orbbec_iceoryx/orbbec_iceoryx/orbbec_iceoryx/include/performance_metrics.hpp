#pragma once

#include <chrono>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace orbbec {

class PublisherMetrics {
public:
    explicit PublisherMetrics(const std::string& serial, double targetFps = 0.0)
        : m_serial(serial), m_targetFps(targetFps) {
        reset();
    }

    void reset() {
        m_startTime = std::chrono::steady_clock::now();
        m_lastFrameTime = std::chrono::steady_clock::time_point();
        m_frameCount = 0;
        m_byteCount = 0;
        m_cameraDrops = 0;
        m_loanFailures = 0;
        m_prevFrameId = 0;

        m_latencySumMs = 0.0;
        m_latencyMinMs = 1e9;
        m_latencyMaxMs = 0.0;

        m_jitterSumMs = 0.0;
        m_jitterSumSqMs = 0.0;
        m_jitterMinMs = 1e9;
        m_jitterMaxMs = 0.0;
        m_jitterCount = 0;

        m_getImageSumMs = 0.0;
        m_pubSumMs = 0.0;
        m_demosaicSumMs = 0.0;
        m_jpegEncodeSumMs = 0.0;
        m_requeueSumMs = 0.0;
    }

    void update(uint64_t frameId, size_t bytes, double latencyMs, bool loanSuccess,
                double getImageMs = 0.0, double pubMs = 0.0, double demosaicMs = 0.0,
                double jpegEncodeMs = 0.0, double requeueMs = 0.0) {
        auto now = std::chrono::steady_clock::now();
        
        if (!loanSuccess) {
            m_loanFailures++;
            return;
        }

        m_frameCount++;
        m_byteCount += bytes;

        // Track camera drops via Frame ID
        if (m_prevFrameId != 0 && frameId > m_prevFrameId + 1) {
            m_cameraDrops += (frameId - m_prevFrameId - 1);
        }
        m_prevFrameId = frameId;

        // Track software latency
        m_latencySumMs += latencyMs;
        m_latencyMinMs = std::min(m_latencyMinMs, latencyMs);
        m_latencyMaxMs = std::max(m_latencyMaxMs, latencyMs);

        // Track pipeline steps
        m_getImageSumMs += getImageMs;
        m_pubSumMs += pubMs;
        m_demosaicSumMs += demosaicMs;
        m_jpegEncodeSumMs += jpegEncodeMs;
        m_requeueSumMs += requeueMs;

        // Track frame interval / jitter
        if (m_lastFrameTime.time_since_epoch().count() > 0) {
            double intervalMs = std::chrono::duration<double, std::milli>(now - m_lastFrameTime).count();
            m_jitterSumMs += intervalMs;
            m_jitterSumSqMs += intervalMs * intervalMs;
            m_jitterMinMs = std::min(m_jitterMinMs, intervalMs);
            m_jitterMaxMs = std::max(m_jitterMaxMs, intervalMs);
            m_jitterCount++;
        }
        m_lastFrameTime = now;
    }

    bool shouldReport(double intervalSec = 2.0) const {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_startTime).count();
        return elapsed >= intervalSec;
    }

    std::string generateReport() {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_startTime).count();
        if (elapsed <= 0.0) return "";

        double fps = static_cast<double>(m_frameCount) / elapsed;
        double mbps = (static_cast<double>(m_byteCount) / (1024.0 * 1024.0)) / elapsed;

        double avgLatency = m_frameCount > 0 ? (m_latencySumMs / static_cast<double>(m_frameCount)) : 0.0;
        double minLatency = m_latencyMinMs < 1e8 ? m_latencyMinMs : 0.0;
        double maxLatency = m_latencyMaxMs;

        double jitterStdDev = 0.0;
        if (m_jitterCount > 0) {
            double avgInterval = m_jitterSumMs / static_cast<double>(m_jitterCount);
            double variance = (m_jitterSumSqMs / static_cast<double>(m_jitterCount)) - (avgInterval * avgInterval);
            jitterStdDev = (variance > 0.0) ? std::sqrt(variance) : 0.0;
        }

        // Compute step averages
        double avgGetImage = m_frameCount > 0 ? (m_getImageSumMs / static_cast<double>(m_frameCount)) : 0.0;
        double avgPub = m_frameCount > 0 ? (m_pubSumMs / static_cast<double>(m_frameCount)) : 0.0;
        double avgDemosaic = m_frameCount > 0 ? (m_demosaicSumMs / static_cast<double>(m_frameCount)) : 0.0;
        double avgJpeg = m_frameCount > 0 ? (m_jpegEncodeSumMs / static_cast<double>(m_frameCount)) : 0.0;
        double avgRequeue = m_frameCount > 0 ? (m_requeueSumMs / static_cast<double>(m_frameCount)) : 0.0;

        // Determine bottleneck step (excluding steps that average 0.0)
        std::string bottleneckName = "None";
        double maxStepAvg = 0.0;

        if (avgGetImage > maxStepAvg) { maxStepAvg = avgGetImage; bottleneckName = "GetImage"; }
        if (avgPub > maxStepAvg) { maxStepAvg = avgPub; bottleneckName = "IceoryxPublish"; }
        if (avgDemosaic > maxStepAvg) { maxStepAvg = avgDemosaic; bottleneckName = "Demosaic"; }
        if (avgJpeg > maxStepAvg) { maxStepAvg = avgJpeg; bottleneckName = "JpegEncode"; }
        if (avgRequeue > maxStepAvg) { maxStepAvg = avgRequeue; bottleneckName = "RequeueBuffer"; }

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "SN " << m_serial << " | "
           << "FPS: " << fps;
        if (m_targetFps > 0.0) {
            ss << " (target: " << m_targetFps << ")";
        }
        ss << " | Throughput: " << mbps << " MB/s"
           << " | Latency: Avg=" << avgLatency << "ms, Min=" << minLatency << ", Max=" << maxLatency << "ms"
           << " | Jitter: " << jitterStdDev << "ms"
           << " | Camera Drops: " << m_cameraDrops
           << " | Loan Failures: " << m_loanFailures;

        if (m_getImageSumMs > 0.0 || m_pubSumMs > 0.0 || m_demosaicSumMs > 0.0 || m_jpegEncodeSumMs > 0.0 || m_requeueSumMs > 0.0) {
            ss << "\n    [Pipeline Averages] GetImage=" << avgGetImage
               << "ms | IceoryxPublish=" << avgPub
               << "ms | Demosaic=" << avgDemosaic
               << "ms | JpegEncode=" << avgJpeg
               << "ms | RequeueBuffer=" << avgRequeue
               << "ms [Bottleneck: " << bottleneckName << "]";
        }

        // Reset for the next interval
        reset();
        return ss.str();
    }

private:
    std::string m_serial;
    double m_targetFps;
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastFrameTime;

    size_t m_frameCount;
    size_t m_byteCount;
    uint64_t m_cameraDrops;
    uint64_t m_loanFailures;
    uint64_t m_prevFrameId;

    double m_latencySumMs;
    double m_latencyMinMs;
    double m_latencyMaxMs;

    double m_jitterSumMs;
    double m_jitterSumSqMs;
    double m_jitterMinMs;
    double m_jitterMaxMs;
    size_t m_jitterCount;

    double m_getImageSumMs;
    double m_pubSumMs;
    double m_demosaicSumMs;
    double m_jpegEncodeSumMs;
    double m_requeueSumMs;
};

class SubscriberMetrics {
public:
    explicit SubscriberMetrics(const std::string& serial)
        : m_serial(serial) {
        reset();
    }

    void reset() {
        m_startTime = std::chrono::steady_clock::now();
        m_lastFrameTime = std::chrono::steady_clock::time_point();
        m_frameCount = 0;
        m_byteCount = 0;
        m_transmissionDrops = 0;
        m_expectedSeq = -1;

        m_writeLatencySumMs = 0.0;
        m_writeLatencyMinMs = 1e9;
        m_writeLatencyMaxMs = 0.0;

        m_e2eLatencySumMs = 0.0;
        m_e2eLatencyMinMs = 1e9;
        m_e2eLatencyMaxMs = 0.0;

        m_jitterSumMs = 0.0;
        m_jitterSumSqMs = 0.0;
        m_jitterMinMs = 1e9;
        m_jitterMaxMs = 0.0;
        m_jitterCount = 0;
    }

    void update(int64_t seq, size_t bytes, double writeLatencyMs, double e2eLatencyMs) {
        auto now = std::chrono::steady_clock::now();

        m_frameCount++;
        m_byteCount += bytes;

        // Track transmission drops via Sequence Number
        if (m_expectedSeq != -1) {
            if (seq != m_expectedSeq) {
                if (seq > m_expectedSeq) {
                    m_transmissionDrops += (seq - m_expectedSeq);
                }
            }
        }
        m_expectedSeq = seq + 1;

        // Track write latency
        m_writeLatencySumMs += writeLatencyMs;
        m_writeLatencyMinMs = std::min(m_writeLatencyMinMs, writeLatencyMs);
        m_writeLatencyMaxMs = std::max(m_writeLatencyMaxMs, writeLatencyMs);

        // Track end-to-end latency
        m_e2eLatencySumMs += e2eLatencyMs;
        m_e2eLatencyMinMs = std::min(m_e2eLatencyMinMs, e2eLatencyMs);
        m_e2eLatencyMaxMs = std::max(m_e2eLatencyMaxMs, e2eLatencyMs);

        // Track reception jitter
        if (m_lastFrameTime.time_since_epoch().count() > 0) {
            double intervalMs = std::chrono::duration<double, std::milli>(now - m_lastFrameTime).count();
            m_jitterSumMs += intervalMs;
            m_jitterSumSqMs += intervalMs * intervalMs;
            m_jitterMinMs = std::min(m_jitterMinMs, intervalMs);
            m_jitterMaxMs = std::max(m_jitterMaxMs, intervalMs);
            m_jitterCount++;
        }
        m_lastFrameTime = now;
    }

    bool shouldReport(double intervalSec = 2.0) const {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_startTime).count();
        return elapsed >= intervalSec;
    }

    std::string generateReport() {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_startTime).count();
        if (elapsed <= 0.0) return "";

        double fps = static_cast<double>(m_frameCount) / elapsed;
        double mbps = (static_cast<double>(m_byteCount) / (1024.0 * 1024.0)) / elapsed;

        double avgWrite = m_frameCount > 0 ? (m_writeLatencySumMs / static_cast<double>(m_frameCount)) : 0.0;
        double minWrite = m_writeLatencyMinMs < 1e8 ? m_writeLatencyMinMs : 0.0;
        double maxWrite = m_writeLatencyMaxMs;

        double avgE2e = m_frameCount > 0 ? (m_e2eLatencySumMs / static_cast<double>(m_frameCount)) : 0.0;
        double minE2e = m_e2eLatencyMinMs < 1e8 ? m_e2eLatencyMinMs : 0.0;
        double maxE2e = m_e2eLatencyMaxMs;

        double jitterStdDev = 0.0;
        if (m_jitterCount > 0) {
            double avgInterval = m_jitterSumMs / static_cast<double>(m_jitterCount);
            double variance = (m_jitterSumSqMs / static_cast<double>(m_jitterCount)) - (avgInterval * avgInterval);
            jitterStdDev = (variance > 0.0) ? std::sqrt(variance) : 0.0;
        }

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "SN " << m_serial << " | "
           << "Recv FPS: " << fps
           << " | Throughput: " << mbps << " MB/s"
           << " | Write Latency: Avg=" << avgWrite << "ms, Min=" << minWrite << ", Max=" << maxWrite << "ms"
           << " | E2E Latency: Avg=" << avgE2e << "ms, Min=" << minE2e << ", Max=" << maxE2e << "ms"
           << " | Jitter: " << jitterStdDev << "ms"
           << " | Transmission Drops: " << m_transmissionDrops;

        // Reset for the next interval
        reset();
        return ss.str();
    }

private:
    std::string m_serial;
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastFrameTime;

    size_t m_frameCount;
    size_t m_byteCount;
    uint64_t m_transmissionDrops;
    int64_t m_expectedSeq;

    double m_writeLatencySumMs;
    double m_writeLatencyMinMs;
    double m_writeLatencyMaxMs;

    double m_e2eLatencySumMs;
    double m_e2eLatencyMinMs;
    double m_e2eLatencyMaxMs;

    double m_jitterSumMs;
    double m_jitterSumSqMs;
    double m_jitterMinMs;
    double m_jitterMaxMs;
    size_t m_jitterCount;
};

} // namespace orbbec
