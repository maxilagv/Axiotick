#pragma once

#include <string>
#include <vector>

namespace argentum::ml {

/**
 * @class PythonBridge
 * @brief ZeroMQ client to request predictions from Python models.
 */
class PythonBridge {
public:
    PythonBridge(const std::string& endpoint) : endpoint_(endpoint) {}

    void connect() {
        // In prod: zmq_connect(socket, endpoint)
    }

    /**
     * @brief Sends features to Python and awaits prediction.
     * @return Predicted price movement (e.g., 1.0 for up, -1.0 for down).
     */
    double predict(const std::vector<double>& features) {
        (void)features;
        // Mock IPC communication
        // send(features)
        // recv(prediction)
        return 0.5; // Dummy confident UP prediction
    }

private:
    std::string endpoint_;
};

} // namespace argentum::ml
