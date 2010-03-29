#include "nfsping.h"

volatile sig_atomic_t quitting;

/* convert a timeval to microseconds */
unsigned long tv2us(struct timeval tv) {
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

/* convert a timeval to milliseconds */
unsigned long tv2ms(struct timeval tv) {
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* convert milliseconds to a timeval */
void ms2tv(struct timeval *tv, unsigned long ms) {
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
}

/* convert milliseconds to a timespec */
void ms2ts(struct timespec *ts, unsigned long ms) {
    ts->tv_sec = ms / 1000;
    ts->tv_nsec = (ms % 1000000) * 1000000;
}

void int_handler(int sig) {
    quitting = 1;
}

void print_summary(targets_t targets) {
    targets_t *target = &targets;
    double loss;

    while (target) {
        loss = (target->sent - target->received) / (double)target->sent * 100;
        printf("%s : xmt/rcv/%%loss = %u/%u/%.0f%%, min/avg/max = %.2f/%.2f/%.2f\n",
            target->name, target->sent, target->received, loss, target->min / 1000.0, target->avg / 1000.0, target->max / 1000.0);
        target = target->next;
    }
}

void print_verbose_summary(targets_t targets) {
    targets_t *target = &targets;
    results_t *current;

    while (target) {
        printf("%s :", target->name);
        current = target->results;
        while (current) {
            if (current->us)
                printf(" %.2f", current->us / 1000.0);
            else
                printf(" -");
            current = current->next;
        }
        printf("\n");
        target = target->next;
    }
}

int main(int argc, char **argv) {
    enum clnt_stat status;
    char *error;
    /* default 2.5 seconds */
    struct timeval timeout = { 2, 500000 };
    struct timeval call_start, call_end;
    /* default 1 second */
    struct timespec sleep_time = { 1, 0 };
    /* default 25 ms */
    struct timespec wait_time = { 0, 25000000 };
    int sock = RPC_ANYSOCK;
    struct addrinfo hints;
    int getaddr;
    unsigned long us;
    double loss;
    targets_t *targets;
    targets_t *target;
    results_t *results;
    results_t *current;
    int ch;
    unsigned long count;
    int verbose = 0, loop = 0, ip = 0, quiet = 0;
    int first, index;

    /* listen for ctrl-c */
    quitting = 0;
    signal(SIGINT, int_handler);

    targets = calloc(1, sizeof(targets_t));
    target = targets;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM; /* change to SOCK_STREAM for TCP */

    while ((ch = getopt(argc, argv, "AC:c:i:lp:qt:")) != -1) {
        switch(ch) {
            case 'A':
                ip = 1;
                break;
            case 'C':
                verbose = 1;
                target->results = calloc(1, sizeof(results_t));
                target->current = target->results;
                /* fall through to regular count */
            case 'c':
                count = strtoul(optarg, NULL, 10);
                break;
            case 'i':
                ms2ts(&wait_time, strtoul(optarg, NULL, 10));
                break;
            case 'l':
                loop = 1;
                break;
            case 'p':
                ms2ts(&sleep_time, strtoul(optarg, NULL, 10));
                break;
            case 'q':
                quiet = 1;
                break;
            case 't':
                /* TODO check for zero */
                ms2tv(&timeout, strtoul(optarg, NULL, 10));
                break;
        }
    }

    /* mark the first non-option argument */
    first = optind;

    for (index = optind; index < argc; index++) {
        if (index > first) {
            target->next = calloc(1, sizeof(targets_t));
            target = target->next;
            target->next = NULL;
        }
        target->name = argv[index];
        target->addr = calloc(1, sizeof(struct addrinfo));
        target->addr->ai_addr = calloc(1, sizeof(struct sockaddr_in));
        target->client_sock = (struct sockaddr_in *) target->addr->ai_addr;
        target->client_sock->sin_family = AF_INET;
        target->client_sock->sin_port = htons(NFS_PORT);

        if (verbose) {
            target->results = calloc(1, sizeof(results_t));
            target->current = target->results;
        }

        /* first try treating the hostname as an IP address */
        if (!inet_pton(AF_INET, target->name, &target->client_sock->sin_addr)) {
            /* if that fails, do a DNS lookup */
            getaddr = getaddrinfo(target->name, "nfs", &hints, &target->addr);
            if (getaddr == 0) {
                target->client_sock->sin_addr = ((struct sockaddr_in *)target->addr->ai_addr)->sin_addr;
                if (ip) {
                    target->name = calloc(1, INET_ADDRSTRLEN);
                    inet_ntop(AF_INET, &target->client_sock->sin_addr, target->name, INET_ADDRSTRLEN);
                }
            } else {
                printf("%s: %s\n", target->name, gai_strerror(getaddr));
                exit(EXIT_FAILURE);
            }
        }

        target->client = clntudp_create(target->client_sock, NFS_PROGRAM, 3, timeout, &sock);
        if (target->client) {
            target->client->cl_auth = authnone_create();
        } else {
            clnt_pcreateerror(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    /* reset back to start of list */
    target = targets;

    while(1) {
        if (quitting) {
            break;
        }

        while (target) {
            gettimeofday(&call_start, NULL);
            status = clnt_call(target->client, NFSPROC_NULL, (xdrproc_t) xdr_void, NULL, (xdrproc_t) xdr_void, error, timeout);
            gettimeofday(&call_end, NULL);
            target->sent++;

            if (status == RPC_SUCCESS) {
                /* check if we're not looping */
                if (!count && !loop) {
                    printf("%s is alive\n", target->name);
                    exit(EXIT_SUCCESS);
                }
                target->received++;
                loss = (target->sent - target->received) / (double)target->sent * 100;

                us = tv2us(call_end) - tv2us(call_start);

                /* first result is a special case */
                if (target->received == 1) {
                    target->min = target->max = target->avg = us;
                } else {
                    if (verbose) {
                        target->current->next = calloc(1, sizeof(results_t));
                        target->current = target->current->next;
                    }
                    if (us < target->min) target->min = us;
                    if (us > target->max) target->max = us;
                    /* calculate the average time */
                    target->avg = (target->avg * (target->received - 1) + us) / target->received;
                }

                if (verbose)
                    target->current->us = us;

                if (!quiet)
                    printf("%s : [%u], %03.2f ms (%03.2f avg, %.0f%% loss)\n", target->name, target->sent - 1, us / 1000.0, target->avg / 1000.0, loss);
            } else {
                clnt_perror(target->client, target->name);
                if (!count && !loop) {
                    printf("%s is dead\n", target->name);
                    exit(EXIT_FAILURE);
                }
                if (verbose && target->sent > 1) {
                    target->current->next = calloc(1, sizeof(results_t));
                    target->current = target->current->next;
                }
            }

            target = target->next;
            if (target)
                nanosleep(&wait_time, NULL);
        }

        /* reset back to start of list */
        /* do this at the end of the loop not the start so we can check if we're done or need to sleep */
        target = targets;

        if (count && target->sent >= count) {
            break;
        }

        nanosleep(&sleep_time, NULL);
    }
    printf("\n");
    if (verbose)
        print_verbose_summary(*targets);
    else
        print_summary(*targets);
    exit(EXIT_SUCCESS);
}
