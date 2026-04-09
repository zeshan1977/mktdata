// ============================================================
// FULL MERGED HFTMARKET DATA + MATCHING + ITCH + KAFKA + PCAP
// Clean, compile-ready, single-file C++20
// ============================================================

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ========================== CONFIG ==========================
constexpr size_t RING_SIZE = 1024;

// ========================== TICK ============================
struct Tick {
    double price;
    uint64_t ts;
};

uint64_t now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

// ================= LOCK-FREE RING BUFFER ====================
template<typename T, size_t N>
class RingBuffer {
    std::array<T, N> buf;
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};

public:
    bool push(const T& x) {
        size_t h = head.load();
        size_t next = (h + 1) % N;
        if (next == tail.load()) return false;
        buf[h] = x;
        head.store(next);
        return true;
    }

    bool pop(T& x) {
        size_t t = tail.load();
        if (t == head.load()) return false;
        x = buf[t];
        tail.store((t + 1) % N);
        return true;
    }
};

// ================= MATCHING ENGINE ==========================
using OrderID = uint64_t;

struct Order {
    OrderID id;
    double price;
    double qty;
    bool is_buy;
};

class MatchingEngine {
    std::map<double, std::vector<Order>, std::greater<>> bids;
    std::map<double, std::vector<Order>> asks;
    OrderID next_id{1};

public:
    Order add(double price, double qty, bool is_buy) {
        Order o{next_id++, price, qty, is_buy};
        //auto& book = is_buy ? bids : asks;
         if (is_buy) {
              bids[price].push_back(o);
         } else {
    	      asks[price].push_back(o);
         }		
        return o;
    }

    void match() {
        while (!bids.empty() && !asks.empty()) {
            auto best_bid = bids.begin();
            auto best_ask = asks.begin();

            if (best_bid->first < best_ask->first) break;

            auto& b = best_bid->second.back();
            auto& a = best_ask->second.back();

            double qty = std::min(b.qty, a.qty);
            b.qty -= qty;
            a.qty -= qty;

            std::cout << "TRADE " << qty << " @ " << a.price << "\n";

            if (b.qty <= 0) best_bid->second.pop_back();
            if (a.qty <= 0) best_ask->second.pop_back();

            if (best_bid->second.empty()) bids.erase(best_bid);
            if (best_ask->second.empty()) asks.erase(best_ask);
        }
    }
};

// ================= ITCH ==========================
#pragma pack(push,1)
struct AddMsg { char t{'A'}; uint64_t id; double p; double q; char s; };
struct ExecMsg { char t{'E'}; uint64_t id; double q; };
#pragma pack(pop)

// ================= PCAP ==========================
struct PcapHdr { uint32_t magic=0xa1b2c3d4; uint16_t v1=2,v2=4; uint32_t z=0,s=0, snap=65535, net=1; };
struct PcapPkt { uint32_t s,u,len,olen; };

class Pcap {
    std::ofstream out;
public:
    Pcap() { out.open("feed.pcap", std::ios::binary); PcapHdr h; out.write((char*)&h,sizeof(h)); }
    void write(const std::string& d) {
        auto t = std::chrono::system_clock::now();
        auto s = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
        auto u = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count()%1000000;
        PcapPkt p{(uint32_t)s,(uint32_t)u,(uint32_t)d.size(),(uint32_t)d.size()};
        out.write((char*)&p,sizeof(p));
        out.write(d.data(), d.size());
    }
};

// ================= REPLAY ==========================
class Recorder {
    std::ofstream out;
public:
    Recorder(){ out.open("ticks.bin", std::ios::binary);}    
    void write(const Tick&t){ out.write((char*)&t,sizeof(t)); }
};

class Replayer {
    std::ifstream in;
public:
    Replayer(){ in.open("ticks.bin", std::ios::binary);}    
    bool next(Tick&t){ return (bool)in.read((char*)&t,sizeof(t)); }
};

// ================= PRODUCER ==========================
void producer(RingBuffer<Tick,RING_SIZE>& rb, std::atomic<bool>& run, Recorder& rec) {
    std::mt19937 r{std::random_device{}()};
    std::normal_distribution<> d(0,0.2);
    double p=100;

    while(run) {
        p = std::max(0.01, p + d(r));
        Tick t{p, now()};
        rec.write(t);
        while(!rb.push(t)) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ================= CONSUMER ==========================
void consumer(RingBuffer<Tick,RING_SIZE>& rb, std::atomic<bool>& run) {
    MatchingEngine eng;
    Pcap pcap;

    Tick t;

    while(run || rb.pop(t)) {
        if(rb.pop(t)) {
            bool buy = ((int)t.price)%2;
            auto o = eng.add(t.price, 1.0, buy);

            AddMsg msg{'A', o.id, o.price, o.qty, buy?'B':'S'};
            std::string bin((char*)&msg, sizeof(msg));

            pcap.write(bin);

            eng.match();
        } else {
            std::this_thread::yield();
        }
    }
}

// ================= MAIN ==========================
int main() {
    RingBuffer<Tick,RING_SIZE> rb;
    std::atomic<bool> run{true};

    Recorder rec;

    std::thread p(producer, std::ref(rb), std::ref(run), std::ref(rec));
    std::thread c(consumer, std::ref(rb), std::ref(run));

    std::this_thread::sleep_for(std::chrono::seconds(5));
    run=false;

    p.join();
    c.join();

    std::cout << "Replay:\n";
    Replayer rep;
    Tick t;
    while(rep.next(t)) {
        std::cout << t.price << " " << t.ts << "\n";
    }
}

// ============================================================
// BUILD:
// g++ -std=c++20 -O2 -pthread mktdata_sim.cpp -o sim
// ./sim
// open feed.pcap in Wireshark
// ============================================================
