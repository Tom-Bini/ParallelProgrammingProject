#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <Average.h>
#include <Player.h>
#include <Map.h>
#include <WindowManager.h>
#include <Raycaster.h>
#include <UDPReceiver.h>
#include <UDPSender.h>
#include <DoubleBuffer.h>
#include <util.h>

std::thread send_position;
std::atomic<bool> running(true);

std::mutex cv_mutex;
std::condition_variable cv;
bool player_moved = false;

struct ProgramArguments
{
    int screenWidth;
    int screenHeight;
    std::string ipsPath;
};

ProgramArguments parseArgs(int argc, char *argv[])
{
    if (argc != 4)
    {
        std::cerr << "Usage: " << argv[0] << " <screenWidth> <screenHeight> <ipsPath>" << std::endl;
        std::cerr << "  screenWidth: The width of the screen." << std::endl;
        std::cerr << "  screenHeight: The height of the screen." << std::endl;
        std::cerr << "  ipsPath: The path to the file containing the IP addresses and ports of the players." << std::endl;
        std::cerr << "Example: " << argv[0] << " 1920 1080 ips.txt" << std::endl;
        exit(1);
    }

    ProgramArguments args;
    args.screenWidth = std::stoi(argv[1]);
    args.screenHeight = std::stoi(argv[2]);
    args.ipsPath = argc == 4 ? argv[3] : "";
    return args;
}

void signalHandler(int) {     // signal handler to join the display_thread when we Ctrl-c  
    running = false;          // we don't exit immediately
}

int main(int argc, char *argv[])
{
    ProgramArguments args = parseArgs(argc, argv);
    const int screenWidth = args.screenWidth;
    const int screenHeight = args.screenHeight;

    std::vector<std::unique_ptr<UDPSender>> udpSenders;
    NetworkData data = parseIPs(args.ipsPath);
    UDPReceiver udpReceiver(data.listeningPort);
    for (auto ipPort : data.ipPorts)
        udpSenders.push_back(std::unique_ptr<UDPSender>(new UDPSender(ipPort.first, ipPort.second)));
    size_t nbPlayers = udpSenders.size();

    // Indexes used to identify other players
    int nextPlayerIndex = 0;
    std::map<std::string, int> playersIndexes; // Maps IP addresses and ports to player indexes

    Map map = Map::generateMap(nbPlayers);
    Player player({22, 11.5}, {-1, 0}, {0, 0.66}, 5, 3, map);
    DoubleBuffer doubleBuffer(screenWidth, screenHeight);
    WindowManager windowManager(doubleBuffer);
    Raycaster raycaster(player, doubleBuffer, map);

    std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now(), oldTime;

    Average fpsCounter(1.0);

    if (signal(SIGINT, signalHandler) == SIG_ERR) {     // Install the signal handler
        std::cerr << "Signal error" << std::endl;
    }

    // Send position to other players
    send_position = std::thread([&udpSenders, &player] () {
        while (true) {
            std::unique_lock<std::mutex> lock(cv_mutex);        // unique_lock because cv needs a controllable mutex

            cv.wait(lock, [] { return player_moved || !running ; });        // don't wait if running is false

            player_moved = false;

            double x = player.posX();
            double y = player.posY();

            lock.unlock();                                // unlock before the network operation

            if (!running) break;

            for (auto &udpSender : udpSenders)
                udpSender->send(x, y);
        }
    });

    while (true)
    {
        raycaster.castFloorCeiling();
        raycaster.castWalls();
        raycaster.castSprites();

        doubleBuffer.swap();

        oldTime = time;
        time = std::chrono::system_clock::now();
        std::chrono::duration<double> elapsed = time - oldTime;
        double frameTime = elapsed.count();

        fpsCounter.update(1.0 / frameTime);
        std::cout << "\r" << std::to_string(int(fpsCounter.get())) << " FPS" << std::flush;

        windowManager.updateDisplay();
        windowManager.updateInput();

        bool moved = false;
        unsigned int keys = windowManager.getKeysPressed();
        if (keys & WindowManager::KEY_UP) {
            player.move(frameTime);
            moved = true;
        }
        if (keys & WindowManager::KEY_DOWN) {
            player.move(-frameTime);
            moved = true;
        }
        if (keys & WindowManager::KEY_RIGHT) {
            player.turn(-frameTime);
        }
        if (keys & WindowManager::KEY_LEFT) {
            player.turn(frameTime);
        }
        if (keys & WindowManager::KEY_ESC) {
            running = false;
        }

        if (moved) {
            {
                std::lock_guard<std::mutex> lock(cv_mutex);
                player_moved = true;
            }
            cv.notify_one();
        }
        
        if (!running) {
            cv.notify_one();
            break;
        }

        // Receive other players' positions and update them
        for (size_t i = 0; i < nbPlayers; i++)
        {
            UDPData data = udpReceiver.receive();
            if (!data.valid)
                break;
            // Update the player's index if it is the first time we receive data from them
            if (playersIndexes.find(data.sender) == playersIndexes.end())
            {
                playersIndexes[data.sender] = nextPlayerIndex++;
                nextPlayerIndex %= nbPlayers;
            }
            int index = playersIndexes[data.sender];
            map.movePlayer(index, data.position.x(), data.position.y());
        }
    }
    send_position.join();
}
