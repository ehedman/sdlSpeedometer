#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_net.h>
#include "sdlSpeedometer.h"

#define MAX_CLIENTS 5

typedef struct {
    TCPsocket sock;
    SDLNet_SocketSet set;
    collected_nmea *cnmea;
} ClientData;

static int client_handler(void *ptr)
{
    ClientData *data = (ClientData *)ptr;
    TCPsocket client_sock = data->sock;
    SDLNet_SocketSet socket_set = data->set;
    collected_nmea *cnmea = data->cnmea;
    free(data);

    cnmea->conf->numThreads++;

    SDL_Log("TCP Server: Attached to a new client");

    while (cnmea->conf->runTcp) {
        // SDLNet_TCP_Send returns the number of bytes sent. If <= 0, the client has disconnected.
        SDL_LockMutex(cnmea->conf->tcp_mutex);
        int bytes_sent = SDLNet_TCP_Send(client_sock, cnmea->tcp_buff, strlen(cnmea->tcp_buff));
        SDL_UnlockMutex(cnmea->conf->tcp_mutex);
        
        if (bytes_sent <= 0) {
            SDL_Log("TCP Server: Client detached");
            break;
        }
        SDL_Delay(20);
    }

    SDLNet_TCP_DelSocket(socket_set, client_sock);
    SDLNet_TCP_Close(client_sock);

    cnmea->conf->numThreads--;
   
    return 0;
}

int threadTcp_server(void *conf)
{
    collected_nmea *cnmea = conf;

    TCPsocket server_sock, client_sock;
    IPaddress ip;
    SDLNet_SocketSet socket_set;

    if (SDL_Init(SDL_INIT_TIMER) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "TCP Server: SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    socket_set = SDLNet_AllocSocketSet(MAX_CLIENTS + 1);
    if (!socket_set) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "TCP Server: SDLNet_AllocSocketSet failed: %s", SDLNet_GetError());
        return 1;
    }

    if (SDLNet_ResolveHost(&ip, NULL, cnmea->conf->runTcp) == -1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "TCP Server: SDLNet_ResolveHost failed: %s", SDLNet_GetError());
        SDLNet_FreeSocketSet(socket_set);
        return 1;
    }

    server_sock = SDLNet_TCP_Open(&ip);
    if (!server_sock) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,"TCP Server: Could not open server port %d: %s", cnmea->conf->runTcp, SDLNet_GetError());
        SDLNet_FreeSocketSet(socket_set);
        return 1;
    }

    cnmea->conf->numThreads++;

    SDLNet_TCP_AddSocket(socket_set, server_sock);

    SDL_Log("TCP Server: Running on port %d", cnmea->conf->runTcp);

    while (cnmea->conf->runTcp) {
        client_sock = SDLNet_TCP_Accept(server_sock);
        
        if (client_sock) {
            SDL_Log("TCP Server: A new client accepted via SDL_net!");

            // Add new client to our shared SocketSet.
            SDLNet_TCP_AddSocket(socket_set, client_sock);

            // Package data for the new thread.
            ClientData *c_data = malloc(sizeof(ClientData));
            c_data->sock = client_sock;
            c_data->set = socket_set;
            c_data->cnmea = cnmea;

            // Create the thread with SDL_CreateThread.
            SDL_Thread *thread = SDL_CreateThread(client_handler, "ClientTransmissionThread", (void *)c_data);
            
            if (!thread) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "TCP Server: Could not create execution thread: %s", SDL_GetError());
                SDLNet_TCP_DelSocket(socket_set, client_sock);
                SDLNet_TCP_Close(client_sock);
                free(c_data);
            } else {
                // Detach the thread in SDL so that resources are automatically cleaned up when `client_handler` returns.
                SDL_DetachThread(thread);
            }
        }
        
        SDL_Delay(50);
    }

    SDLNet_TCP_Close(server_sock);
    SDLNet_FreeSocketSet(socket_set);

    SDL_Log("TCP Server: Terminated");

    cnmea->conf->numThreads--;

    return 0;
}
