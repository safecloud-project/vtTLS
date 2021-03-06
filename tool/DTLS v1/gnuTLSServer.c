/* This example code is placed in the public domain. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <gnutls/gnutls.h>
#include <gnutls/dtls.h>
#include <assert.h>
#include <unistd.h>

#define KEYFILE "certs/x509-server-key.pem"
#define CERTFILE "certs/x509-server.pem"
#define CAFILE "certs/x509-ca.pem"
#define CRLFILE "crl.pem"

/* This is a sample DTLS echo server, using X.509 authentication.
 * Note that error checking is minimal to simplify the example.
 */

// Error checking
#define CHECK(x) assert((x)>=0)
#define CHK_ERR(err,s) if ((err)==-1) { perror(s); exit(1); }
#define CHK_GNUTLS(err) if ((err)==-1) { perror("CHK_GNUTLS"); exit(2); }

#define CHK_RET(ret) note(ret)

void note (int ret){
	if (ret < 0 && gnutls_error_is_fatal(ret) == 0) {
        	fprintf(stderr, "*** Warning: %s\n", gnutls_strerror(ret));
        } else if (ret < 0) {
                fprintf(stderr, "Error in recv(): %s\n", gnutls_strerror(ret));
        }

        if (ret == 0) {
                printf("EOF");
        }
}

// Buffer settings
#define BUF_SZ 1024
#define MAX_FILE_NAME 512

// 2^32=2147483647 plus negative plus thousands separators plus null character
#define MAX_INT_STRING 10+1+3+1

#define PORT 5557

typedef struct {
        gnutls_session_t session;
        int fd;
        struct sockaddr *cli_addr;
        socklen_t cli_addr_size;
} priv_data_st;

static int pull_timeout_func(gnutls_transport_ptr_t ptr, unsigned int ms);
static ssize_t push_func(gnutls_transport_ptr_t p, const void *data, size_t size);
static ssize_t pull_func(gnutls_transport_ptr_t p, void *data, size_t size);
static const char *human_addr(const struct sockaddr *sa, socklen_t salen, char *buf, size_t buflen);
static int wait_for_connection(int fd);

/* Use global credentials and parameters to simplify
 * the example. */
static gnutls_certificate_credentials_t x509_cred;
static gnutls_priority_t priority_cache;

int main(void)
{
        int listen_sd;
        int sock, ret;
        struct sockaddr_in sa_serv;
        struct sockaddr_in cli_addr;
        socklen_t cli_addr_size;
	priv_data_st priv;
        gnutls_datum_t cookie_key;
        gnutls_dtls_prestate_st prestate;
        gnutls_session_t session;
	int mtu = 1500;
        char buffer[BUF_SZ];

	/* buffer for file name */
	char file_name[MAX_FILE_NAME];

	/* buffer for file size as string */
	char file_len_str[MAX_INT_STRING];

        /* this must be called once in the program */
        gnutls_global_init();

        gnutls_certificate_allocate_credentials(&x509_cred);
        gnutls_certificate_set_x509_trust_file(x509_cred, CAFILE,
                                               GNUTLS_X509_FMT_PEM);

        gnutls_certificate_set_x509_crl_file(x509_cred, CRLFILE,
                                             GNUTLS_X509_FMT_PEM);

        ret =
            gnutls_certificate_set_x509_key_file(x509_cred, CERTFILE,
                                                 KEYFILE,
                                                 GNUTLS_X509_FMT_PEM);
        if (ret < 0) {
                printf("No certificate or key were found\n");
                exit(1);
        }

        gnutls_certificate_set_known_dh_params(x509_cred, GNUTLS_SEC_PARAM_MEDIUM);

        gnutls_priority_init(&priority_cache,
                             "PERFORMANCE:-VERS-TLS-ALL:+VERS-DTLS1.0:%SERVER_PRECEDENCE",
                             NULL);

        gnutls_key_generate(&cookie_key, GNUTLS_COOKIE_KEY_SIZE);

        /* Socket operations
         */
        listen_sd = socket(AF_INET, SOCK_DGRAM, 0);

        memset(&sa_serv, '\0', sizeof(sa_serv));
        sa_serv.sin_family = AF_INET;
        sa_serv.sin_addr.s_addr = inet_addr("192.168.1.1");
        //sa_serv.sin_addr.s_addr = inet_addr("10.0.1.100");
        sa_serv.sin_port = htons(PORT);

        {                       /* DTLS requires the IP don't fragment (DF) bit to be set */
#if defined(IP_DONTFRAG)
                int optval = 1;
                setsockopt(listen_sd, IPPROTO_IP, IP_DONTFRAG,
                           (const void *) &optval, sizeof(optval));
#elif defined(IP_MTU_DISCOVER)
                int optval = IP_PMTUDISC_DO;
                setsockopt(listen_sd, IPPROTO_IP, IP_MTU_DISCOVER,
                           (const void *) &optval, sizeof(optval));
#endif
        }

        bind(listen_sd, (struct sockaddr *) &sa_serv, sizeof(sa_serv));

        printf("UDP server ready. Listening to port '%d'.\n\n", PORT);

        for (;;) {
                printf("Waiting for connection...\n");
                sock = wait_for_connection(listen_sd);
                if (sock < 0)
                        continue;

                cli_addr_size = sizeof(cli_addr);
                ret = recvfrom(sock, buffer, sizeof(buffer), MSG_PEEK,
                               (struct sockaddr *) &cli_addr,
                               &cli_addr_size);
                if (ret > 0) {
                        memset(&prestate, 0, sizeof(prestate));
                        ret =
                            gnutls_dtls_cookie_verify(&cookie_key,
                                                      &cli_addr,
                                                      sizeof(cli_addr),
                                                      buffer, ret,
                                                      &prestate);
                        if (ret < 0) {  /* cookie not valid */
                                priv_data_st s;

                                memset(&s, 0, sizeof(s));
                                s.fd = sock;
                                s.cli_addr = (void *) &cli_addr;
                                s.cli_addr_size = sizeof(cli_addr);

                                printf
                                    ("Sending hello verify request to %s\n",
                                     human_addr((struct sockaddr *)
                                                &cli_addr,
                                                sizeof(cli_addr), buffer,
                                                sizeof(buffer)));

                                gnutls_dtls_cookie_send(&cookie_key,
                                                        &cli_addr,
                                                        sizeof(cli_addr),
                                                        &prestate,
                                                        (gnutls_transport_ptr_t)
                                                        & s, push_func);

                                /* discard peeked data */
                                recvfrom(sock, buffer, sizeof(buffer), 0,
                                         (struct sockaddr *) &cli_addr,
                                         &cli_addr_size);
                                usleep(100);
                                continue;
                        }
                        printf("Accepted connection from %s\n",
                               human_addr((struct sockaddr *)
                                          &cli_addr, sizeof(cli_addr),
                                          buffer, sizeof(buffer)));
                } else
                        continue;

                gnutls_init(&session, GNUTLS_SERVER | GNUTLS_DATAGRAM);
                gnutls_priority_set(session, priority_cache);
                gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE,
                                       x509_cred);

                gnutls_dtls_prestate_set(session, &prestate);
                gnutls_dtls_set_mtu(session, mtu);

                priv.session = session;
                priv.fd = sock;
                priv.cli_addr = (struct sockaddr *) &cli_addr;
                priv.cli_addr_size = sizeof(cli_addr);

                gnutls_transport_set_ptr(session, &priv);
                gnutls_transport_set_push_function(session, push_func);
                gnutls_transport_set_pull_function(session, pull_func);
                gnutls_transport_set_pull_timeout_function(session,
                                                           pull_timeout_func);

                do {
                        ret = gnutls_handshake(session);
                }
                while (ret == GNUTLS_E_INTERRUPTED || ret == GNUTLS_E_AGAIN);
                /* Note that DTLS may also receive GNUTLS_E_LARGE_PACKET.
                 * In that case the MTU should be adjusted.
                 */

                if (ret < 0) {
                        fprintf(stderr, "Error in handshake(): %s\n",
                                gnutls_strerror(ret));
                        gnutls_deinit(session);
                        continue;
                }

                printf("- Handshake was completed\n");

		/* ----------------------------------------------- */
		/* ---------------- DATA EXCHANGE ---------------- */
		/* ----------------------------------------------- */
	
		// REQUEST

		// read file name line (result includes line break)
		ret = gnutls_record_recv(session, buffer, BUF_SZ);
		CHK_RET(ret);

		// copy file name from work buffer, removing line break
		strncpy(file_name, buffer, ret);
		file_name[ret] = '\0';
		printf("The file name required by the client is: %s\n", file_name);

		// read empty line separator
		ret = gnutls_record_recv(session, buffer, BUF_SZ);
		CHK_RET(ret);

		// open file to read
		FILE *file;
		long file_len;

		// Open the file in binary mode
		file = fopen(file_name, "rb");

		if (file == NULL) {
			printf("\tClient requested file that does not exist\n");
			// -1 means that the file does not exist
			file_len = -1;
		} else {
			printf("\tFile open\n");
			fseek(file, 0, SEEK_END);	// Jump to the end of the file
			file_len = ftell(file);		// Get the current byte offset in the file
			rewind(file);			// Jump back to the beginning of the file
			printf("\tFile size is %ld\n", file_len);
		}

		// RESPONSE 

		// write file size (as a string)
		sprintf(file_len_str, "%ld", file_len);
		ret = gnutls_record_send(session, file_len_str, strlen(file_len_str));

		// add end-of-line
		ret = gnutls_record_send(session, "\n", sizeof(char));
		printf("Wrote file size\n");

		// write empty line separator
		ret = gnutls_record_send(session, "\n", sizeof(char));
		printf("Wrote separator line\n");

		if (file_len > 0) {
			printf("\t\tReturning secure data to client\n");
					
			// write file to secure socket
			int bytesRead = 0;

			while (bytesRead < file_len) {
				int readResult = fread(buffer, sizeof(char), BUF_SZ, file);
				CHK_ERR(readResult, "file");
				printf("%d - Read %d bytes from file\n", i, readResult);
				bytesRead += readResult;

				int writeResult = gnutls_record_send(session, buffer, readResult);
				CHK_RET(writeResult);
				printf("Wrote %d bytes to secure socket\n", writeResult);
			}
			printf("\t\tWrote all file bytes\n");
			// Close the file
			fclose(file);
			printf("\tFile closed\n");
		}		
		/* It is suggested not to use GNUTLS_SHUT_RDWR in DTLS
         	* connections because the peer's closure message might
         	* be lost */
                gnutls_bye(session, GNUTLS_SHUT_WR);
                gnutls_deinit(session);

        }
        close(listen_sd);

        gnutls_certificate_free_credentials(x509_cred);
        gnutls_priority_deinit(priority_cache);

        gnutls_global_deinit();

        return 0;

}



static int wait_for_connection(int fd)
{
        fd_set rd, wr;
        int n;

        FD_ZERO(&rd);
        FD_ZERO(&wr);

        FD_SET(fd, &rd);

        /* waiting part */
        n = select(fd + 1, &rd, &wr, NULL, NULL);
        if (n == -1 && errno == EINTR)
                return -1;
        if (n < 0) {
                perror("select()");
                exit(1);
        }

        return fd;
}

/* Wait for data to be received within a timeout period in milliseconds
 */
static int pull_timeout_func(gnutls_transport_ptr_t ptr, unsigned int ms)
{
        fd_set rfds;
        struct timeval tv;
        priv_data_st *priv = ptr;
        struct sockaddr_in cli_addr;
        socklen_t cli_addr_size;
        int ret;
        char c;

        FD_ZERO(&rfds);
        FD_SET(priv->fd, &rfds);

        tv.tv_sec = 0;
        tv.tv_usec = ms * 1000;

        while (tv.tv_usec >= 1000000) {
                tv.tv_usec -= 1000000;
                tv.tv_sec++;
        }

        ret = select(priv->fd + 1, &rfds, NULL, NULL, &tv);

        if (ret <= 0)
                return ret;

        /* only report ok if the next message is from the peer we expect
         * from 
         */
        cli_addr_size = sizeof(cli_addr);
        ret =
            recvfrom(priv->fd, &c, 1, MSG_PEEK,
                     (struct sockaddr *) &cli_addr, &cli_addr_size);
        if (ret > 0) {
                if (cli_addr_size == priv->cli_addr_size
                    && memcmp(&cli_addr, priv->cli_addr,
                              sizeof(cli_addr)) == 0)
                        return 1;
        }

        return 0;
}

static ssize_t
push_func(gnutls_transport_ptr_t p, const void *data, size_t size)
{
        priv_data_st *priv = p;

        return sendto(priv->fd, data, size, 0, priv->cli_addr,
                      priv->cli_addr_size);
}

static ssize_t pull_func(gnutls_transport_ptr_t p, void *data, size_t size)
{
        priv_data_st *priv = p;
        struct sockaddr_in cli_addr;
        socklen_t cli_addr_size;
        char buffer[64];
        int ret;

        cli_addr_size = sizeof(cli_addr);
        ret =
            recvfrom(priv->fd, data, size, 0,
                     (struct sockaddr *) &cli_addr, &cli_addr_size);
        if (ret == -1)
                return ret;

        if (cli_addr_size == priv->cli_addr_size
            && memcmp(&cli_addr, priv->cli_addr, sizeof(cli_addr)) == 0)
                return ret;

        printf("Denied connection from %s\n",
               human_addr((struct sockaddr *)
                          &cli_addr, sizeof(cli_addr), buffer,
                          sizeof(buffer)));

        gnutls_transport_set_errno(priv->session, EAGAIN);
        return -1;
}

static const char *human_addr(const struct sockaddr *sa, socklen_t salen,
                              char *buf, size_t buflen)
{
        const char *save_buf = buf;
        size_t l;

        if (!buf || !buflen)
                return NULL;

        *buf = '\0';

        switch (sa->sa_family) {
#if HAVE_IPV6
        case AF_INET6:
                snprintf(buf, buflen, "IPv6 ");
                break;
#endif

        case AF_INET:
                snprintf(buf, buflen, "IPv4 ");
                break;
        }

        l = strlen(buf);
        buf += l;
        buflen -= l;

        if (getnameinfo(sa, salen, buf, buflen, NULL, 0, NI_NUMERICHOST) !=
            0)
                return NULL;

        l = strlen(buf);
        buf += l;
        buflen -= l;

        strncat(buf, " port ", buflen);

        l = strlen(buf);
        buf += l;
        buflen -= l;

        if (getnameinfo(sa, salen, NULL, 0, buf, buflen, NI_NUMERICSERV) !=
            0)
                return NULL;

        return save_buf;
}

