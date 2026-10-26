// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <optional>
#include <unordered_map>
#include <boost/serialization/export.hpp>
#include "core/hle/service/service.h"
#include "core/hle/service/sm/notification_queue.h"

namespace Core {
class System;
struct TimingEventType;
}

namespace Kernel {
class HLERequestContext;
class Semaphore;
} // namespace Kernel

namespace Network::ArticBase { class Client; }

namespace Service::SM {

/// Interface to "srv:" service
class SRV final : public ServiceFramework<SRV> {
public:
    explicit SRV(Core::System& system);
    ~SRV();

    class ThreadCallback;
    void StartArticCartridgeMonitor(std::shared_ptr<Network::ArticBase::Client> client);
    void StopArticCartridgeMonitor();
    void PublishNotification(u32 id, bool once = true);

private:
    void RegisterClient(Kernel::HLERequestContext& ctx);
    void EnableNotification(Kernel::HLERequestContext& ctx);
    void GetServiceHandle(Kernel::HLERequestContext& ctx);
    void Subscribe(Kernel::HLERequestContext& ctx);
    void ReceiveNotification(Kernel::HLERequestContext& ctx);
    void Unsubscribe(Kernel::HLERequestContext& ctx);
    void PublishToSubscriber(Kernel::HLERequestContext& ctx);
    void RegisterService(Kernel::HLERequestContext& ctx);

    struct Notifications {
        std::shared_ptr<Kernel::Semaphore> semaphore;
        NotificationQueue queue;
        template <class Archive> void serialize(Archive& ar, const unsigned int) {
            ar & semaphore; ar & queue;
        }
    };
    std::unordered_map<u32, Notifications> notifications;
    std::shared_ptr<Network::ArticBase::Client> cartridge_client;
    std::jthread cartridge_thread;
    std::mutex cartridge_mutex;
    std::condition_variable_any cartridge_wake;
    std::optional<std::optional<u64>> cartridge_mailbox;
    std::optional<std::optional<u64>> pending_cartridge;
    Core::TimingEventType* cartridge_event = nullptr;
    void UpdateCartridge();

    Core::System& system;
    std::shared_ptr<Kernel::Semaphore> notification_semaphore;
    std::unordered_map<std::string, std::shared_ptr<Kernel::Event>> get_service_handle_delayed_map;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
    friend class boost::serialization::access;
};

} // namespace Service::SM

SERVICE_CONSTRUCT(Service::SM::SRV)
BOOST_CLASS_EXPORT_KEY(Service::SM::SRV)
BOOST_CLASS_EXPORT_KEY(Service::SM::SRV::ThreadCallback)
