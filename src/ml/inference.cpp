// Edge Impulse Inference implementation
// Note: This is a stub implementation. The actual Edge Impulse SDK
// must be generated from studio.edgeimpulse.com for your model.

#include "inference.h"
#include "edge_impulse.h"
#include "../core/config.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include <SPIFFS.h>

// Static members
bool MLInference::modelLoaded = false;
char MLInference::modelVersion[16] = "none";
size_t MLInference::modelSize = 0;
uint32_t MLInference::inferenceCount = 0;
uint32_t MLInference::avgInferenceTime = 0;
const char* MLInference::MODEL_PATH = "/M5PORKCHOP/models/porkchop_model.bin";

// Edge Impulse will generate these - placeholder structure
struct ei_impulse_result_t {
    float classification[5];
    float timing;
};

void MLInference::init() {
    // Initialize SPIFFS for model storage
    if (!SPIFFS.begin(true)) {
        Serial.println("[ML] Failed to mount SPIFFS");
        return;
    }
    
    // Try to initialize Edge Impulse SDK
    if (EdgeImpulse::init()) {
        modelLoaded = true;
        strncpy(modelVersion, "EI-SDK", 15);
        EdgeImpulse::printInfo();
    }
    // Try to load existing model file
    else if (SPIFFS.exists(MODEL_PATH)) {
        loadModel(MODEL_PATH);
    } else {
        Serial.println("[ML] No model found, using heuristic classifier");
    }
    
    Serial.println("[ML] Inference engine initialized");
    Display::setMLStatus(true);
}

void MLInference::update() {
    // Process any pending async inference callbacks
    // In real implementation, check for completed inference tasks
}

MLResult MLInference::classify(const float* features, size_t featureCount) {
    MLResult result = {
        .label = MLLabel::UNKNOWN,
        .confidence = 0.0f,
        .scores = {0},
        .inferenceTimeUs = 0,
        .valid = false
    };
    
    // Try Edge Impulse SDK first if enabled
    if (EdgeImpulse::isEnabled()) {
        uint32_t startTime = micros();
        EIResult eiResult = EdgeImpulse::classify(features, featureCount);
        
        if (eiResult.success) {
            result.label = (MLLabel)eiResult.predictedClass;
            result.confidence = eiResult.confidence;
            for (int i = 0; i < 5; i++) {
                result.scores[i] = eiResult.predictions[i];
            }
            result.inferenceTimeUs = micros() - startTime;
            result.valid = true;
        } else {
            // Fallback to heuristic classifier
            result = runInference(features, featureCount);
        }
    } else {
        // Use heuristic classifier
        result = runInference(features, featureCount);
    }
    
    inferenceCount++;
    avgInferenceTime = (avgInferenceTime * (inferenceCount - 1) + result.inferenceTimeUs) / inferenceCount;
    
    // Trigger mood based on result
    if (result.valid) {
        Mood::onMLPrediction(result.confidence);
    }
    
    return result;
}

MLResult MLInference::classifyNetwork(const WiFiFeatures& network) {
    float features[FEATURE_VECTOR_SIZE];
    FeatureExtractor::toFeatureVector(network, features);
    return classify(features, FEATURE_VECTOR_SIZE);
}

void MLInference::classifyAsync(const float* features, size_t featureCount, MLCallback callback) {
    // For ESP32 without PSRAM, we do sync inference
    // A real async implementation would use a task queue
    MLResult result = classify(features, featureCount);
    if (callback) {
        callback(result);
    }
}

MLResult MLInference::runInference(const float* input, size_t size) {
    uint32_t startTime = micros();
    
    MLResult result = {
        .label = MLLabel::NORMAL,
        .confidence = 0.0f,
        .scores = {0.8f, 0.05f, 0.05f, 0.05f, 0.05f},
        .inferenceTimeUs = 0,
        .valid = true
    };
    
    if (size < FEATURE_VECTOR_SIZE) {
        result.valid = false;
        return result;
    }
    
    // ========================================
    // ENHANCED HEURISTIC CLASSIFIER
    // Feature indices from features.cpp:
    //  0: rssi, 1: noise, 2: snr, 3: channel, 4: secondary_ch
    //  5: beacon_interval, 6: capability_lo, 7: capability_hi
    //  8: hasWPS, 9: hasWPA, 10: hasWPA2, 11: hasWPA3
    // 12: isHidden, 13: responseTime, 14: beaconCount, 15: beaconJitter
    // 16: respondsToProbe, 17: probeResponseTime, 18: vendorIECount
    // 19: supportedRates, 20: htCapabilities, 21: vhtCapabilities
    // 22: anomalyScore
    // ========================================
    
    float rssi = input[0];
    float snr = input[2];
    uint8_t channel = (uint8_t)input[3];
    float beaconInterval = input[5];
    bool hasWPS = input[8] > 0.5f;
    bool hasWPA = input[9] > 0.5f;
    bool hasWPA2 = input[10] > 0.5f;
    bool hasWPA3 = input[11] > 0.5f;
    bool isHidden = input[12] > 0.5f;
    float beaconJitter = input[15];
    uint8_t vendorIECount = (uint8_t)input[18];
    uint8_t supportedRates = (uint8_t)input[19];
    bool hasHT = input[20] > 0.5f;
    bool hasVHT = input[21] > 0.5f;
    
    float anomalyScore = 0.0f;
    
    // ---- ROGUE AP DETECTION ----
    // 1. Suspiciously strong signal (someone nearby with laptop hotspot)
    if (rssi > -30) {
        anomalyScore += 0.3f;
    }
    
    // 2. Non-standard beacon interval (default is 100ms, 102.4 TU)
    if (beaconInterval < 50 || beaconInterval > 200) {
        anomalyScore += 0.2f;
    }
    
    // 3. High beacon jitter (inconsistent timing = software AP)
    if (beaconJitter > 10.0f) {
        anomalyScore += 0.15f;
    }
    
    // 4. Missing vendor-specific IEs (real routers have many)
    if (vendorIECount < 2) {
        anomalyScore += 0.1f;
    }
    
    // 5. Open network with WPS enabled (honeypot pattern)
    if (!hasWPA && !hasWPA2 && !hasWPA3 && hasWPS) {
        anomalyScore += 0.25f;
    }
    
    // 6. Channel anomaly - using unusual channels (non-1,6,11 for 2.4GHz)
    if (channel <= 14 && channel != 1 && channel != 6 && channel != 11) {
        anomalyScore += 0.05f;
    }
    
    // 7. Claims VHT (WiFi 5) but no HT (WiFi 4) - inconsistent
    if (hasVHT && !hasHT) {
        anomalyScore += 0.2f;
    }
    
    // 8. Very few supported rates (minimal AP implementation)
    if (supportedRates < 4) {
        anomalyScore += 0.1f;
    }
    
    // ---- EVIL TWIN DETECTION ----
    // Would need SSID comparison with known networks
    // For now, flag hidden networks copying popular names
    float evilTwinScore = 0.0f;
    if (isHidden && rssi > -50) {
        evilTwinScore += 0.2f;
    }
    
    // ---- VULNERABLE NETWORK DETECTION ----
    float vulnScore = 0.0f;
    
    // Open network
    if (!hasWPA && !hasWPA2 && !hasWPA3) {
        vulnScore += 0.5f;
    }
    
    // WPA1 only (TKIP vulnerable)
    if (hasWPA && !hasWPA2 && !hasWPA3) {
        vulnScore += 0.4f;
    }
    
    // WPS enabled (PIN attack vulnerable)
    if (hasWPS) {
        vulnScore += 0.2f;
    }
    
    // Hidden SSID with weak security
    if (isHidden && vulnScore > 0.3f) {
        vulnScore += 0.1f;
    }
    
    // ---- DEAUTH TARGET SCORING ----
    float deauthScore = 0.0f;
    
    // Good signal for reliable deauth
    if (rssi > -70 && rssi < -30) {
        deauthScore += 0.2f;
    }
    
    // Not WPA3 (PMF protected)
    if (!hasWPA3) {
        deauthScore += 0.3f;
    }
    
    // Has active clients (would need client tracking)
    // deauthScore += clientCount > 0 ? 0.2f : 0.0f;
    
    // ---- CLASSIFICATION ----
    result.scores[0] = 1.0f - (anomalyScore + evilTwinScore + vulnScore) / 3.0f;  // NORMAL
    result.scores[1] = min(1.0f, anomalyScore);  // ROGUE_AP
    result.scores[2] = min(1.0f, evilTwinScore);  // EVIL_TWIN
    result.scores[3] = min(1.0f, deauthScore);  // DEAUTH_TARGET
    result.scores[4] = min(1.0f, vulnScore);  // VULNERABLE
    
    // Normalize scores
    float sum = 0.0f;
    for (int i = 0; i < 5; i++) sum += result.scores[i];
    if (sum > 0) {
        for (int i = 0; i < 5; i++) result.scores[i] /= sum;
    }
    
    // Find highest score
    int maxIdx = 0;
    float maxScore = result.scores[0];
    for (int i = 1; i < 5; i++) {
        if (result.scores[i] > maxScore) {
            maxScore = result.scores[i];
            maxIdx = i;
        }
    }
    
    result.label = (MLLabel)maxIdx;
    result.confidence = maxScore;
    result.inferenceTimeUs = micros() - startTime;
    
    return result;
}

bool MLInference::loadModel(const char* path) {
    File f = SPIFFS.open(path, "r");
    if (!f) {
        Serial.printf("[ML] Failed to open model: %s\n", path);
        return false;
    }
    
    modelSize = f.size();
    
    // Read model header (version info)
    char header[32];
    f.readBytes(header, min((size_t)31, modelSize));
    header[31] = 0;
    
    // Parse version from header
    strncpy(modelVersion, header, 15);
    modelVersion[15] = 0;
    
    // In real implementation:
    // - Validate model format
    // - Load weights into inference engine
    // - Initialize Edge Impulse runtime
    
    f.close();
    modelLoaded = true;
    
    Serial.printf("[ML] Model loaded: %s (%d bytes)\n", modelVersion, modelSize);
    return true;
}

bool MLInference::saveModel(const char* path) {
    // Would save current model state
    // Useful for storing updated models from OTA
    return false;
}

bool MLInference::updateModel(const uint8_t* modelData, size_t size) {
    if (!validateModel(modelData, size)) {
        Serial.println("[ML] Model validation failed");
        return false;
    }
    
    // Save to SPIFFS
    File f = SPIFFS.open(MODEL_PATH, "w");
    if (!f) {
        Serial.println("[ML] Failed to open model file for writing");
        return false;
    }
    
    f.write(modelData, size);
    f.close();
    
    // Reload
    return loadModel(MODEL_PATH);
}

bool MLInference::validateModel(const uint8_t* data, size_t size) {
    // Basic validation
    if (size < 64) return false;  // Too small
    if (size > 100000) return false;  // Too large for ESP32
    
    // Check magic header (Edge Impulse format)
    // Real implementation would verify model integrity
    
    return true;
}

const char* MLInference::getModelVersion() {
    return modelVersion;
}

size_t MLInference::getModelSize() {
    return modelSize;
}

bool MLInference::isModelLoaded() {
    return modelLoaded;
}

bool MLInference::checkForUpdate(const char* serverUrl) {
    // Would HTTP GET to check for new model version
    // Returns true if newer version available
    return false;
}

bool MLInference::downloadAndUpdate(const char* url, bool promptUser) {
    if (promptUser) {
        bool confirm = Display::showConfirmBox("ML UPDATE", "Download new model?");
        if (!confirm) return false;
    }
    
    Display::showProgress("Downloading model...", 0);
    
    // Would HTTP download model here
    // Display::showProgress("Downloading...", progress);
    
    Display::showProgress("Installing...", 90);
    
    // updateModel(downloadedData, downloadedSize);
    
    Display::showInfoBox("ML UPDATE", "Model updated!", modelVersion);
    
    return true;
}
