// XCX's online layer (NPLN, gRPC/WebRTC over nn::socket and nn::ssl, nn::friends) on a host that is always offline.
// The libraries initialize; every socket fails with ENETDOWN, name lookups fail, SSL contexts and friend queries
// return errors. nifm already reports no network (ov_system.c), so the game should not try, and when it does the
// failures look like a console without a connection. The byte-order and address-text helpers stay lifted.
#include <stdint.h>
#include <string.h>

#include "host.h"

#define BSD_ENETDOWN 50
#define EAI_FAIL 4
#define SSL_R_UNAVAILABLE 0x27bu       // module 123 (ssl), description 1
#define FRIENDS_R_UNAVAILABLE 0x279u   // module 121 (friends), description 1

static void ov_ok(Cpu *c) { ret_x(c, 0); }            // Initialize, Finalize: Result success
static void ov_nop(Cpu *c) { (void)c; }
static void ov_sock_fail(Cpu *c) { ret_x(c, (uint64_t)-1); }   // int calls: -1, errno from GetLastErrno
static void ov_errno(Cpu *c) { ret_x(c, BSD_ENETDOWN); }
static void ov_gai_fail(Cpu *c) { ret_x(c, EAI_FAIL); }
static void ov_ssl_fail(Cpu *c) { ret_x(c, SSL_R_UNAVAILABLE); }
static void ov_friends_fail(Cpu *c) { ret_x(c, FRIENDS_R_UNAVAILABLE); }

static void ov_socket_initialize(Cpu *c) {
    rt_log("socket: offline host: sockets fail with ENETDOWN");
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", ov_socket_initialize);
HOST_OVERRIDE("_ZN2nn6socket8FinalizeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn6socket12GetLastErrnoEv", ov_errno);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", ov_gai_fail);
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", ov_nop);
HOST_OVERRIDE("_ZN2nn6socket6SocketEiii", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket5CloseEi", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket8ShutdownEii", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket4BindEiPK8sockaddrj", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket7ConnectEiPK8sockaddrj", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket6ListenEii", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket6AcceptEiP8sockaddrPj", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket10SetSockOptEiiiPKvj", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket10GetSockOptEiiiPvPj", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket4SendEiPKvmi", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket4RecvEiPvmi", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket4ReadEiPvm", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket5WriteEiPKvm", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket4PollEPNS0_6PollFdEmi", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket4PollEP6pollfdmi", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket11EventFdReadEiPm", ov_sock_fail);
HOST_OVERRIDE("_ZN2nn6socket12EventFdWriteEim", ov_sock_fail);

HOST_OVERRIDE("_ZN2nn3ssl10InitializeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn3ssl8FinalizeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ov_ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ov_ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ov_ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ov_ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ov_ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj",
              ov_ssl_fail);

HOST_OVERRIDE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",
              ov_friends_fail);
HOST_OVERRIDE("_ZN2nn7friends14AddPlayHistoryERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameES6_", ov_ok);
HOST_OVERRIDE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", ov_friends_fail);
HOST_OVERRIDE("_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi",
              ov_friends_fail);

// The C entry points the gRPC and WebRTC libraries use: the same offline answers.
static void ov_null(Cpu *c) { ret_x(c, 0); }   // GetHostByNameWithOptions: no hostent
HOST_OVERRIDE("nnsocketGetLastErrno", ov_errno);
HOST_OVERRIDE("nnsocketSetLastErrno", ov_nop);
HOST_OVERRIDE("nnsocketGetAddrInfo", ov_gai_fail);
HOST_OVERRIDE("nnsocketFreeAddrInfo", ov_nop);
HOST_OVERRIDE("nnsocketGetHostByNameWithOptions", ov_null);
HOST_OVERRIDE("nnsocketSocket", ov_sock_fail);
HOST_OVERRIDE("nnsocketClose", ov_sock_fail);
HOST_OVERRIDE("nnsocketShutdown", ov_sock_fail);
HOST_OVERRIDE("nnsocketBind", ov_sock_fail);
HOST_OVERRIDE("nnsocketConnect", ov_sock_fail);
HOST_OVERRIDE("nnsocketListen", ov_sock_fail);
HOST_OVERRIDE("nnsocketAccept", ov_sock_fail);
HOST_OVERRIDE("nnsocketGetSockName", ov_sock_fail);
HOST_OVERRIDE("nnsocketGetPeerName", ov_sock_fail);
HOST_OVERRIDE("nnsocketSetSockOpt", ov_sock_fail);
HOST_OVERRIDE("nnsocketGetSockOpt", ov_sock_fail);
HOST_OVERRIDE("nnsocketSend", ov_sock_fail);
HOST_OVERRIDE("nnsocketRecv", ov_sock_fail);
HOST_OVERRIDE("nnsocketSendMsg", ov_sock_fail);
HOST_OVERRIDE("nnsocketRecvMsg", ov_sock_fail);
HOST_OVERRIDE("nnsocketFcntl", ov_sock_fail);
HOST_OVERRIDE("nnsocketPoll", ov_sock_fail);
HOST_OVERRIDE("nnsocketSysctl", ov_sock_fail);
HOST_OVERRIDE("nnsocketResolverSetOption", ov_sock_fail);
HOST_OVERRIDE("nnsocketResolverGetOption", ov_sock_fail);

HOST_OVERRIDE("nnsslInitialize", ov_ok);
HOST_OVERRIDE("nnsslFinalize", ov_ok);
HOST_OVERRIDE("nnsslContextGetContextId", ov_ssl_fail);
HOST_OVERRIDE("nnsslContextDestroy", ov_ok);
HOST_OVERRIDE("nnsslConnectionCreate", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionDestroy", ov_ok);
HOST_OVERRIDE("nnsslConnectionSetOption", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetSocketDescriptor", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetHostName", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetVerifyOption", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetSessionCacheMode", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetIoMode", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetNextAlpnProto", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionGetNextAlpnProto", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionDoHandshakeWithCertBuffer", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionGetVerifyCertError", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionGetConnectionId", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionRead", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionWrite", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionPeek", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionPending", ov_ssl_fail);

// nsd: the production environment ("lp1"); no name resolves.
#define NSD_R_UNAVAILABLE 0x28du   // module 141 (nsd), description 1
static void ov_nsd_environment(Cpu *c) {
    char *out = arg_p(c, 0);
    if (out) memcpy(out, "lp1\0\0\0\0", 8);
}
static void ov_nsd_fail(Cpu *c) { ret_x(c, NSD_R_UNAVAILABLE); }
HOST_OVERRIDE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", ov_nsd_environment);
HOST_OVERRIDE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", ov_nsd_fail);
