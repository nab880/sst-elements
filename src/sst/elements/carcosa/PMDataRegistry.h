// Copyright 2009-2025 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2025, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef CARCOSA_PMDATAREGISTRY_H
#define CARCOSA_PMDATAREGISTRY_H

#include <sst/core/event.h>
#include <map>
#include <string>
#include <vector>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <cstdint>

namespace SST {
namespace Carcosa {

/**
 * PMData - Flexible structure to hold parsed Port Module command data
 *
 * Supports commands with multiple parameters of various types.
 * Parameters are stored as strings and converted on-demand via template accessors.
 *
 * Example formats:
 *   "injection_rate 0.5"           -> command="injection_rate", params=["0.5"]
 *   "set_range 0.1 0.9"            -> command="set_range", params=["0.1", "0.9"]
 *   "config 0x1000 64 true"        -> command="config", params=["0x1000", "64", "true"]
 */
struct PMData {
    std::string command;              // Command name
    std::vector<std::string> params;  // Parameters as strings

    // Constructors
    PMData() : command("") {}
    PMData(const std::string& cmd) : command(cmd) {}
    PMData(const std::string& cmd, const std::vector<std::string>& p) : command(cmd), params(p) {}

    /**
     * Get number of parameters
     */
    size_t paramCount() const { return params.size(); }

    /**
     * Check if parameter exists at index
     */
    bool hasParam(size_t index) const { return index < params.size(); }

    /**
     * Get parameter at index, converted to type T
     * @throws std::out_of_range if index is invalid
     * @throws std::invalid_argument if conversion fails
     */
    template<typename T>
    T getParam(size_t index) const {
        if (index >= params.size()) {
            throw std::out_of_range("PMData::getParam: index " + std::to_string(index) +
                                    " out of range (size=" + std::to_string(params.size()) + ")");
        }
        return convertParam<T>(params[index]);
    }

    /**
     * Get parameter at index with default value if not present or conversion fails
     */
    template<typename T>
    T getParamOr(size_t index, T defaultValue) const {
        if (index >= params.size()) {
            return defaultValue;
        }
        try {
            return convertParam<T>(params[index]);
        } catch (...) {
            return defaultValue;
        }
    }

    /**
     * Backward compatibility: get first parameter as double (or 0.0 if not present)
     */
    double getValue() const {
        return getParamOr<double>(0, 0.0);
    }

    /**
     * Parse a string into PMData
     * Format: "command [param1 [param2 [...]]]"
     *
     * @param str The string to parse (e.g., "injection_rate 0.5" or "set_range 0.1 0.9")
     * @return PMData with command and parameters extracted
     */
    static PMData parse(const std::string& str) {
        PMData data;
        std::istringstream iss(str);

        // First token is the command
        if (!(iss >> data.command)) {
            return data;  // Empty string -> empty PMData
        }

        // Remaining tokens are parameters
        std::string param;
        while (iss >> param) {
            data.params.push_back(param);
        }

        return data;
    }

private:
    /**
     * Convert string to type T - primary template (uses stringstream)
     */
    template<typename T>
    static T convertParam(const std::string& str) {
        T value;
        std::istringstream iss(str);
        if (!(iss >> value)) {
            throw std::invalid_argument("PMData: cannot convert '" + str + "'");
        }
        return value;
    }
};

// Template specializations for common types

template<>
inline std::string PMData::convertParam<std::string>(const std::string& str) {
    return str;
}

template<>
inline double PMData::convertParam<double>(const std::string& str) {
    try {
        return std::stod(str);
    } catch (...) {
        throw std::invalid_argument("PMData: cannot convert '" + str + "' to double");
    }
}

template<>
inline float PMData::convertParam<float>(const std::string& str) {
    try {
        return std::stof(str);
    } catch (...) {
        throw std::invalid_argument("PMData: cannot convert '" + str + "' to float");
    }
}

template<>
inline int PMData::convertParam<int>(const std::string& str) {
    try {
        return std::stoi(str, nullptr, 0);  // base 0 allows hex (0x) and octal (0)
    } catch (...) {
        throw std::invalid_argument("PMData: cannot convert '" + str + "' to int");
    }
}

template<>
inline long PMData::convertParam<long>(const std::string& str) {
    try {
        return std::stol(str, nullptr, 0);
    } catch (...) {
        throw std::invalid_argument("PMData: cannot convert '" + str + "' to long");
    }
}

template<>
inline unsigned long PMData::convertParam<unsigned long>(const std::string& str) {
    try {
        return std::stoul(str, nullptr, 0);
    } catch (...) {
        throw std::invalid_argument("PMData: cannot convert '" + str + "' to unsigned long");
    }
}

template<>
inline long long PMData::convertParam<long long>(const std::string& str) {
    try {
        return std::stoll(str, nullptr, 0);
    } catch (...) {
        throw std::invalid_argument("PMData: cannot convert '" + str + "' to long long");
    }
}

template<>
inline unsigned long long PMData::convertParam<unsigned long long>(const std::string& str) {
    try {
        return std::stoull(str, nullptr, 0);
    } catch (...) {
        throw std::invalid_argument("PMData: cannot convert '" + str + "' to unsigned long long");
    }
}

template<>
inline bool PMData::convertParam<bool>(const std::string& str) {
    // Accept "true", "1", "yes" as true; everything else is false
    return (str == "true" || str == "1" || str == "yes" ||
            str == "True" || str == "TRUE" || str == "Yes" || str == "YES");
}

/**
 * ManagerMessage - Message from a PortModule to a FaultInjManager (via PMDataRegistry queue).
 */
struct ManagerMessage {
    enum class Type { RegisterPM };
    Type type;
    std::string pmId;

    ManagerMessage() : type(Type::RegisterPM), pmId("") {}
    ManagerMessage(Type t, const std::string& id) : type(t), pmId(id) {}
    static ManagerMessage makeRegisterPM(const std::string& id) {
        return ManagerMessage(Type::RegisterPM, id);
    }
};

/**
 * PMDataRegistry - Instance-only registry for Port Module data.
 *
 * Each FaultInjManager owns one instance. Maps event IDs to PM command strings.
 * Also holds a ManagerQueue: messages from PortModules to the Manager (e.g. RegisterPM).
 * Resolve registries by id via PMRegistryResolver (separate type).
 */
class PMDataRegistry {
public:
    PMDataRegistry() = default;

    /** Register PM data for an event ID */
    void registerPMData(SST::Event::id_type id, const std::string& data) {
        registry_[id] = data;
    }

    /** Check if PM data exists for an event ID */
    bool hasPMData(SST::Event::id_type id) const {
        return registry_.find(id) != registry_.end();
    }

    /** Look up raw PM data string for an event ID */
    std::string lookupRaw(SST::Event::id_type id) const {
        auto it = registry_.find(id);
        return (it != registry_.end()) ? it->second : "";
    }

    /** Look up and parse PM data for an event ID */
    PMData lookupPMData(SST::Event::id_type id) const {
        auto it = registry_.find(id);
        return (it != registry_.end()) ? PMData::parse(it->second) : PMData();
    }

    /** Clear PM data for a specific event ID */
    void clearPMData(SST::Event::id_type id) {
        registry_.erase(id);
    }

    /** Clear all PM data from the registry */
    void clearAll() {
        registry_.clear();
    }

    /** ManagerQueue: enqueue a message from a PortModule to the Manager */
    void pushMessageToManager(const ManagerMessage& msg) {
        managerQueue_.push(msg);
    }

    /** ManagerQueue: drain and return all messages; queue is cleared */
    std::vector<ManagerMessage> popAllMessagesFromPMs() {
        std::vector<ManagerMessage> out;
        while (!managerQueue_.empty()) {
            out.push_back(managerQueue_.front());
            managerQueue_.pop();
        }
        return out;
    }

private:
    std::map<SST::Event::id_type, std::string> registry_;
    std::queue<ManagerMessage> managerQueue_;
};

/**
 * PMRegistryResolver - Maps registry id (string) to PMDataRegistry*.
 * FaultInjManager registers its registry in ctor; FaultInjectorMemH looks up by id.
 */
class PMRegistryResolver {
public:
    static void registerRegistry(const std::string& id, PMDataRegistry* reg) {
        registryMap_[id] = reg;
    }

    static PMDataRegistry* getRegistry(const std::string& id) {
        auto it = registryMap_.find(id);
        return (it != registryMap_.end()) ? it->second : nullptr;
    }

    static void unregisterRegistry(const std::string& id) {
        registryMap_.erase(id);
    }

private:
    static std::map<std::string, PMDataRegistry*> registryMap_;
};

inline std::map<std::string, PMDataRegistry*> PMRegistryResolver::registryMap_;

} // namespace Carcosa
} // namespace SST

#endif // CARCOSA_PMDATAREGISTRY_H
