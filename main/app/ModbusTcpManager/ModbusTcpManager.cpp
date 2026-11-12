#include "ModbusTcpManager.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "HvacSimManager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>

using namespace modbus;

namespace {

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool Due(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }

constexpr uint16_t PROTOCOL_MODBUS = 0;
constexpr int BACKLOG = 2;

/// The registers a request reaches, from its PDU: false for a function that names none
/// this side can place, or a PDU too short to say. A count of 0 still places it; the
/// unit then refuses it as Modbus has it.
bool RequestRegisters(const uint8_t* pdu, size_t len, uint16_t& start, uint16_t& count)
{
    switch (pdu[0])
    {
    case fc::READ_COILS:
    case fc::READ_DISCRETE_INPUTS:
    case fc::READ_HOLDING_REGISTERS:
    case fc::READ_INPUT_REGISTERS:
    case fc::WRITE_MULTIPLE_COILS:
    case fc::WRITE_MULTIPLE_REGISTERS:
        if (len < 5)
            return false;
        start = Get16(pdu + 1);
        count = Get16(pdu + 3);
        return true;
    case fc::WRITE_SINGLE_COIL:
    case fc::WRITE_SINGLE_REGISTER:
        if (len < 3)
            return false;
        start = Get16(pdu + 1);
        count = 1;
        return true;
    default:
        return false;
    }
}

size_t Exception(uint8_t function, uint8_t code, uint8_t* reply)
{
    reply[0] = function | fc::EXCEPTION;
    reply[1] = code;
    return 2;
}

} // namespace

// ── Commands ─────────────────────────────────────────────────

CommandEntry ModbusTcpManager::statusCommand_{
    "mbtcp status", &InvokeCommand<&ModbusTcpManager::Cmd_Status>,
    "Report the Modbus TCP server that plays a CoolMasterNet: the port it listens on (0 "
    "when it does not: mbtcp.port 0 or mbtcp.answer false), the unit id it answers to (0: "
    "any, as a real one), the "
    "connections open now, and counters since boot of connections accepted and refused, "
    "requests, replies, exceptions among them, requests for registers no simulated unit "
    "has (noUnit), for a unit taken off the bus (absent), to another unit id (otherUnit), "
    "and connections dropped for sending something other than Modbus."
};

ModbusTcpManager::ModbusTcpManager(AppProvider& app)
    : app_(app)
{
}

void ModbusTcpManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    StruxProvider& strux = app_.getStrux();
    strux.getSettingsManager().Register({ &portSetting_, &unitSetting_, &answerSetting_ });
    strux.getCommandManager().Register(this, { &statusCommand_ });

    // With the RS485 task: a gateway waits seconds for an answer, nothing here is urgent.
    task_.Init("mbtcp", 5, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
        ESP_LOGE(TAG, "Failed to start the Modbus TCP task");

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

// ── The task ─────────────────────────────────────────────────

void ModbusTcpManager::TaskLoop()
{
    uint32_t nextRefreshMs = 0;
    for (;;)
    {
        const uint32_t now = NowMs();
        if (Due(now, nextRefreshMs))
        {
            nextRefreshMs = now + REFRESH_MS;
            Refresh();
        }

        if (listener_ < 0 && !AnyClient())
        {
            vTaskDelay(pdMS_TO_TICKS(SELECT_MS));
            continue;
        }

        fd_set readable;
        FD_ZERO(&readable);
        int maxFd = -1;
        if (listener_ >= 0)
        {
            FD_SET(listener_, &readable);
            maxFd = listener_;
        }
        for (Client& c : clients_)
        {
            if (c.fd < 0)
                continue;
            FD_SET(c.fd, &readable);
            maxFd = std::max(maxFd, c.fd);
        }

        timeval wait{ .tv_sec = 0, .tv_usec = SELECT_MS * 1000 };
        const int ready = select(maxFd + 1, &readable, nullptr, nullptr, &wait);
        const uint32_t after = NowMs();
        if (ready < 0)
        {
            ESP_LOGW(TAG, "select: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(SELECT_MS));
        }
        else if (ready > 0)
        {
            if (listener_ >= 0 && FD_ISSET(listener_, &readable))
                Accept(after);
            for (Client& c : clients_)
                if (c.fd >= 0 && FD_ISSET(c.fd, &readable))
                    Receive(c, after);
        }

        for (Client& c : clients_)
            if (c.fd >= 0 && after - c.lastMs > IDLE_CLOSE_MS)
                Drop(c);
    }
}

void ModbusTcpManager::Refresh()
{
    // NVS reads, once a second: a setting applies within that.
    const uint32_t port = std::min<uint32_t>(portSetting_.Get(), 0xFFFF);
    const uint32_t unit = std::min<uint32_t>(unitSetting_.Get(), 0xFF);
    const bool answers = answerSetting_.Get();
    unit_ = static_cast<uint8_t>(unit);
    answers_ = answers;

    const uint16_t want = answers ? static_cast<uint16_t>(port) : 0;
    if (want == port_)
        return;

    // Another port, or none: the old server is gone, connections and all.
    CloseListener();
    for (Client& c : clients_)
        if (c.fd >= 0)
            Drop(c);
    if (want != 0)
        Listen(want);
}

void ModbusTcpManager::Listen(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(fd, BACKLOG) < 0)
    {
        // Tried again at the next refresh.
        ESP_LOGE(TAG, "Port %u: errno %d", static_cast<unsigned>(port), errno);
        close(fd);
        return;
    }

    listener_ = fd;
    port_ = port;
    ESP_LOGI(TAG, "A CoolMasterNet on port %u", static_cast<unsigned>(port));
}

void ModbusTcpManager::CloseListener()
{
    if (listener_ < 0)
        return;
    close(listener_);
    listener_ = -1;
    port_ = 0;
    ESP_LOGI(TAG, "Port closed");
}

bool ModbusTcpManager::AnyClient() const
{
    for (const Client& c : clients_)
        if (c.fd >= 0)
            return true;
    return false;
}

void ModbusTcpManager::Accept(uint32_t nowMs)
{
    sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    const int fd = accept(listener_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    if (fd < 0)
        return;

    Client* slot = nullptr;
    for (Client& c : clients_)
        if (c.fd < 0)
        {
            slot = &c;
            break;
        }
    if (slot == nullptr)
    {
        // A real one has its limit too.
        counters_.refused++;
        close(fd);
        return;
    }

    // A request is one small frame and its reply another; nothing to batch.
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    slot->fd = fd;
    slot->len = 0;
    slot->lastMs = nowMs;
    counters_.accepted++;
    clientCount_++;
}

void ModbusTcpManager::Drop(Client& c)
{
    close(c.fd);
    c.fd = -1;
    c.len = 0;
    clientCount_--;
}

void ModbusTcpManager::Receive(Client& c, uint32_t nowMs)
{
    const int n = recv(c.fd, c.buf + c.len, sizeof(c.buf) - c.len, 0);
    if (n <= 0)
    {
        // Closed by the other side (a gateway after its poll), or broken.
        Drop(c);
        return;
    }
    c.len += static_cast<size_t>(n);
    c.lastMs = nowMs;

    // Every whole frame in the buffer, in order; a part of one waits for the rest.
    size_t used = 0;
    while (c.len - used >= MBAP)
    {
        const uint8_t* adu = c.buf + used;
        const uint16_t protocol = Get16(adu + 2);
        const uint16_t length = Get16(adu + 4);   // the unit id and the PDU
        if (protocol != PROTOCOL_MODBUS || length < 2 || length > 1 + MAX_PDU)
        {
            // Not Modbus: whoever this is, it is not a gateway.
            counters_.dropped++;
            Drop(c);
            return;
        }
        const size_t frame = 6 + length;
        if (c.len - used < frame)
            break;
        if (!Answer(c, adu, frame, nowMs))
        {
            Drop(c);
            return;
        }
        used += frame;
    }

    if (used > 0 && used < c.len)
        memmove(c.buf, c.buf + used, c.len - used);
    c.len -= used;
}

bool ModbusTcpManager::Answer(Client& c, const uint8_t* adu, size_t len, uint32_t nowMs)
{
    counters_.requests++;
    const uint8_t unit = adu[6];
    const uint8_t* pdu = adu + MBAP;
    const size_t pduLen = len - MBAP;

    uint8_t out[MBAP + MAX_PDU];
    uint8_t* reply = out + MBAP;
    size_t n = 0;
    uint32_t delayMs = 0;

    const uint8_t want = unit_;
    if (want != 0 && unit != want)
    {
        counters_.otherUnit++;
        n = Exception(pdu[0], ex::GATEWAY_TARGET_FAILED, reply);
    }
    else
    {
        uint16_t start = 0, count = 0;
        if (!RequestRegisters(pdu, pduLen, start, count))
        {
            n = Exception(pdu[0], ex::ILLEGAL_FUNCTION, reply);
        }
        else if (HvacSimulator* sim = app_.getHvacSimManager().FindByRegisters(HvacTransport::ModbusTcp, start, count))
        {
            if (sim->Answers())
            {
                n = sim->Handle(pdu, pduLen, false, reply, nowMs);
                delayMs = sim->ReplyDelayMs();
            }
            else
            {
                // Off the bus: a CoolMasterNet that has no such indoor unit any more.
                counters_.absent++;
                n = Exception(pdu[0], ex::ILLEGAL_DATA_ADDRESS, reply);
            }
        }
        else
        {
            counters_.noUnit++;
            n = Exception(pdu[0], ex::ILLEGAL_DATA_ADDRESS, reply);
        }
    }

    // Nothing to say (a write the type does not answer): the gateway waits, as on RTU.
    if (n == 0)
        return true;
    if (reply[0] & fc::EXCEPTION)
        counters_.exceptions++;

    // Late on purpose: rounded up to whole ticks, so 1 ms is still late.
    if (delayMs != 0)
        vTaskDelay(pdMS_TO_TICKS(delayMs + portTICK_PERIOD_MS - 1));

    // The header back as it came, with the reply's length.
    memcpy(out, adu, 4);
    Put16(out + 4, static_cast<uint16_t>(1 + n));
    out[6] = unit;
    const size_t total = MBAP + n;
    if (send(c.fd, out, total, 0) != static_cast<int>(total))
        return false;
    counters_.answered++;
    return true;
}

// ── Command handlers ─────────────────────────────────────────

CommandResult ModbusTcpManager::Cmd_Status(CommandContext& ctx)
{
    auto resp = ctx.reply.object();
    resp.field("port", static_cast<uint32_t>(port_.load()));
    resp.field("listening", port_.load() != 0);
    resp.field("unit", static_cast<uint32_t>(unit_.load()));
    resp.field("answers", answers_.load());
    resp.field("clients", clientCount_.load());
    auto c = resp.object("counters");
    c.field("accepted", counters_.accepted.load());
    c.field("refused", counters_.refused.load());
    c.field("requests", counters_.requests.load());
    c.field("answered", counters_.answered.load());
    c.field("exceptions", counters_.exceptions.load());
    c.field("noUnit", counters_.noUnit.load());
    c.field("absent", counters_.absent.load());
    c.field("otherUnit", counters_.otherUnit.load());
    c.field("dropped", counters_.dropped.load());
    return CommandResult::Ok;
}
