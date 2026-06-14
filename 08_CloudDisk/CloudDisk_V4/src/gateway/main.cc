#include "CloudDiskServer.h"
#include <iostream>
#include <signal.h>

using namespace std;

WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

int main()
{
    signal(SIGINT, sig_handler);
    srand(time(NULL));

    CloudDiskServer server;

    server.register_routes();

    if (server.start(8888) == 0) {
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
    }
}
