#ifdef __cplusplus
extern "C" {
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "tcpserver.h"

#include "logme.h"
#include "vutils.h"
#include "vlist.h"
#include "httputils.h"
#include "httpparser.h"
#include "macros.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <MSWSock.h>
#include <ntstatus.h>
#include <bcrypt.h>
#include <time.h>
#include <limits.h>

#define DEFAULT_RECV_TIMEOUT_S 15
#define DEFAULT_SEND_TIMEOUT_S 15

typedef struct tcp_node {
	VLISTNODE
		HANDLE handle;
	DWORD tid;
	SOCKET socket;
	int open;
	long recv_timeout_s;
	long send_timeout_s;
} tcp_node;
typedef tcp_node node;
typedef struct file_handle {
	HANDLE handle;
} file_handle;

static int init_winsock() {
	WSADATA wsaData;

	// Initialize Winsock
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		LogMe.et("WSAStartup failed: %d", iResult);
	}
	else
	{
		LogMe.it("WSAStartup succeeded");
	}
	return iResult;
}

typedef struct params {
	node* node_p;
	vlist http_handlers;
	const char* phrase_200;
	const char* html_200;
	const char* phrase_400;
	const char* html_400;
	const char* phrase_500;
	const char* html_500;
} params;

typedef struct tcp_server {
	vlist connections_list;
	int has_open;
} tcp_server;

// return non-zero to break
static int check_cth(vlist this, long i, void* extra) {
	tcp_server* server = extra;
	server->has_open += ((node*)(this->get_const(this, i)))->open;
	return 0;
}

static int all_closed(tcp_server *server){
	server->has_open = 0;
	if (server->connections_list != NULL)
	{
		server->connections_list->foreach(server->connections_list, check_cth, server);
	}
	return !(server->has_open);
}

static int clean_up_connection(node *connection_p, params *params_p, int returned) {
	if (closesocket(connection_p->socket) != 0) {
		LogMe.et("closesocket( %p ) [tid = %lu ] failed with error: %d", connection_p->socket, connection_p->tid, WSAGetLastError());
	}
	if (!CloseHandle(connection_p->handle))
	{
		LogMe.et("CloseHandle( %p ) [tid = %lu ] failed with error: %lu", connection_p->handle, connection_p->tid, GetLastError());
	}
	LogMe.nt("Connection thread [tid = %lu ] [client socket = %p ] exit.", connection_p->tid, connection_p->socket);
	connection_p->open = 0;
	free(params_p);
	return returned;
}

static char16_t *get_utf_16_file_name_from_utf8(const char* u8fn) {
	int wnum = test_wide_char_num_of_utf8_including_wide_null(u8fn);
	char16_t* w_fn = malloc(sizeof(char16_t) * wnum);
	if (w_fn == NULL)
	{
		return NULL;
	}
	utf8_to_utf16_must_have_sufficient_buffer_including_wide_null(u8fn, w_fn, wnum);
	return w_fn;
}

// 获取文件句柄，返回 NULL 表示失败，否则返回指定文件的句柄
static file_handle get_file_hd(const char *filename, int read_only_1_or_write_only_0) {
	const char* prefix = "\\\\?\\";
	const char* suffix = "";
	char* fn_temp = zero_malloc(strlen(filename) + strlen(prefix) + strlen(suffix) + 1);
	if (fn_temp == NULL)
	{
		LogMe.et("Open file [ %s ] failed with error: Malloc Fail", filename);
		return (file_handle) { .handle = NULL };
	}
	strcat(fn_temp, prefix);
	strcat(fn_temp, filename);
	strcat(fn_temp, suffix);
	char16_t* wide_filename = get_utf_16_file_name_from_utf8(fn_temp);
	free(fn_temp); fn_temp = NULL;
	if (wide_filename == NULL)
	{
		LogMe.et("Open file [ %s ] failed with error: Malloc Fail", filename);
		return (file_handle){.handle=NULL};
	}
	HANDLE res = CreateFileW(
		wide_filename,
		read_only_1_or_write_only_0?GENERIC_READ:GENERIC_WRITE,			// 只读（只写）
		read_only_1_or_write_only_0?FILE_SHARE_READ:0,					// 可以和其它进程一起读（读共享）（不能共享）
		NULL,															// lpSecurityAttributes 参数的默认值
		read_only_1_or_write_only_0?OPEN_EXISTING:CREATE_ALWAYS,		// 只有存在时才打开，否则失败（存在则清空，不存在则创建）
		FILE_ATTRIBUTE_NORMAL,											// 普通文件
		NULL															// hTemplateFile 参数的默认值
	);
	free(wide_filename); wide_filename = NULL;
	if (res == INVALID_HANDLE_VALUE)
	{
		DWORD last_err = GetLastError();
		LogMe.et("Open file [ %s ] failed with error: %lu", filename, last_err);
		return (file_handle) { .handle = NULL };
	}
	else
	{
		return (file_handle) { .handle = res };
	}
}

// 不想再收发数据时，请调用此函数来主动关闭连接。
// 调用此函数后无法再调用 recv() 或 send()。
// 调用此函数意味着退出线程。
static int active_shutdown(node *cnt_p, params *params_p, int returned) {
	LogMe.it("active_shutdown( %p )", cnt_p->socket);
	// 主动关闭连接意味着我们不想再收发数据，但关闭连接前应该保证我们先前想要发送的数据已被发送。
	// 等待本机发送缓冲区内的数据都发送完后，按照TCP协议，友善地主动发送 FIN 向对方表明我们想关闭连接。对方收到 FIN 后，我们的写资源会自动释放。
	if (shutdown(cnt_p->socket, SD_SEND) == SOCKET_ERROR) {
		// 如果出错了，说明对方已不可达，接下来释放写资源。由于不会再收到对方发送的数据且不再关心未处理的数据，接下来释放读资源。
		LogMe.et("actively shutdown( %p , SD_SEND ) [tid = %lu ] failed with error: %d", cnt_p->socket, cnt_p->tid, WSAGetLastError());
	}
	else
	{
		// 如果没出错，写资源已被释放。按照TCP协议，对方尚未发送 FIN，我们本应等待对方继续发送的数据和 FIN，但对方可能
		// 永远都不会发送 FIN（不管对方是否是出于恶意），而且我们也不再关心未处理的数据和对方继续发送的数据，因此接下来释放读资源。
		// 读资源释放后，对方会收到 “连接已重置” 的错误。
	}
	// 释放 读资源+写资源，释放 socket。
	return clean_up_connection(cnt_p, params_p, returned);
}

// 当 recv() 函数返回 0 时，对方表明不再向我们发送数据并想要关闭连接。请不要再调用 recv()，并尽快发送完需要发送的数据，然后调用此函数来关闭连接。
// 调用此函数后无法再调用 recv() 或 send()。
// 调用此函数意味着退出线程。
static int recv_0_shutdown(node* cnt_p, params* params_p, int returned) {
	LogMe.bt("recv_0_shutdown( %p )", cnt_p->socket);
	// 因为 recv() 函数表明我们接收到对方的 FIN，意味着对方不会再发送数据且我们也已经处理完对方发来的所有数据，所以释放读资源。
	if (shutdown(cnt_p->socket, SD_RECEIVE) == SOCKET_ERROR) {
		LogMe.et("recv_0 shutdown( %p , SD_RECEIVE ) [tid = %lu ] failed with error: %d", cnt_p->socket, cnt_p->tid, WSAGetLastError());
	}
	// 是时候关闭连接了，不要让对方久等。
	// 关闭连接前应该保证我们先前想要发送的数据已被发送。
	// 等待本机发送缓冲区内的数据都发送完后，按照TCP协议，友善地向对方发送 FIN，表明我们已经发送完需要发送的数据。对方收到 FIN 后，我们的写资源会自动释放。
	if (shutdown(cnt_p->socket, SD_SEND) == SOCKET_ERROR) {
		// 如果出错了，说明对方已不可达，接下来释放写资源。
		LogMe.et("recv_0 shutdown( %p , SD_SEND ) [tid = %lu ] failed with error: %d", cnt_p->socket, cnt_p->tid, WSAGetLastError());
	}
	else
	{
		// 如果没出错，写资源已被释放。
	}
	// 释放 读资源+写资源，释放 socket。
	return clean_up_connection(cnt_p, params_p, returned);
}

// send() 或 recv() 发生错误时，请不要再进行任何 socket 操作，立即调用此函数来关闭连接。
// 调用此函数后无法再调用 recv() 或 send()。
// 调用此函数意味着退出线程。
static int error_shutdown(node* cnt_p, params* params_p, int returned) {
	LogMe.et("error_shutdown( %p )", cnt_p->socket);
	// send() 或 recv() 发生错误意味着对方已不可达，无法进行任何更多的读写操作，并且我们也已经处理完对方发来的所有数据。
	// 此时直接释放 读资源+写资源。
	// 
	// 释放 读资源+写资源，释放 socket。
	return clean_up_connection(cnt_p, params_p, returned);
}

int recv_t(tcp_node *np, char *buf, int len, int flags) {
	ioctlsocket(np->socket, FIONBIO, &((u_long) {0})); // 0:blocking 1:non-blocking
	setsockopt(np->socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&((DWORD) { ((DWORD)(np->recv_timeout_s))*1000 }), sizeof(DWORD));
	int r_res = recv(np->socket, buf, len, flags);
	if (r_res > 0)
	{
		// 通常会以极小的 len 调用此函数，因此为了避免打印太多的冗余日志，暂不打印成功消息
		//LogMe.it("call recv() on socket [ %p ] with len=%d and return=%d", np->socket, len, r_res);
	}
	else if (r_res == 0)
	{
		LogMe.bt("call recv() on socket [ %p ] and recv 0", np->socket);
	}
	else if (r_res == SOCKET_ERROR)
	{
		LogMe.et("call recv() on socket [ %p ] with len=%d and return=SOCKET_ERROR <WSAGetLastError()=%d>", np->socket, len, WSAGetLastError());
	}
	return r_res;
}

int send_t(tcp_node *np, const char *buf, int len, int flags) {
	ioctlsocket(np->socket, FIONBIO, &((u_long) { 0 })); // 0:blocking 1:non-blocking
	setsockopt(np->socket, SOL_SOCKET, SO_SNDTIMEO, (char*)&((DWORD) { ((DWORD)(np->send_timeout_s)) * 1000 }), sizeof(DWORD));
	int s_res = send(np->socket, buf, len, flags);
	if (s_res != SOCKET_ERROR)
	{
		LogMe.it("call send() on socket [ %p ] with len=%d and return=%d", np->socket, len, s_res);
	}
	else
	{
		LogMe.et("call send() on socket [ %p ] with len=%d and return=SOCKET_ERROR <WSAGetLastError()=%d>", np->socket, len, WSAGetLastError());
	}
	return s_res;
}

// 此函数先将 node 结构体中的 socket 设置为阻塞模式，然后通过 socket 传输文件。
// 成功返回 0，失败返回 non-zero。
// 若失败，查看日志以获取详细信息。
static int transmit_file(node* np, HANDLE hFile, unsigned long long file_size, const char* filename) {
	const unsigned long long max_size = 2147483646ULL;
	// blocking mode
	ioctlsocket(np->socket, FIONBIO, &((u_long) { 0 })); // 0:blocking 1:non-blocking
	while (file_size > 0ULL)
	{
		unsigned long long trans_size = file_size > max_size ? max_size : file_size;
		BOOL res = TransmitFile(
			np->socket,
			hFile,
			trans_size,
			0,
			NULL, // blocking mode
			NULL,
			0
		);
		if (res == STATUS_DEVICE_NOT_READY)
		{
			LogMe.et("transmit_file() failed on socket [ %p ] [ file = \"%s\" ] with error: STATUS_DEVICE_NOT_READY", np->socket, filename);
			return 1;
		} else if (!res)
		{	
			int last_err = WSAGetLastError();
			//if (last_err == WSA_IO_PENDING || last_err == ERROR_IO_PENDING)
			//{} else {
				LogMe.et("transmit_file() failed on socket [ %p ] [ file = \"%s\" ] with error: %d", np->socket, filename, last_err);
				return 2;
			//}
		}
		file_size -= trans_size;
	}
	LogMe.it("transmit_file() completed on socket [ %p ] [ file = \"%s\" ]", np->socket, filename);
	return 0;
}

int send_text(tcp_node* np, int status_code, const char * reason_phrase, int keep_alive, const char* text_body_str_could_be_NULL, const char* MIME_type, const char *text_body_charset, int is_download, const char *download_filename) {
	char resp[5000];
	http_response(
		resp,
		sizeof(resp),
		status_code,
		reason_phrase,
		keep_alive,
		NULL,
		text_body_str_could_be_NULL ? strlen(text_body_str_could_be_NULL) : 0,
		MIME_type,
		text_body_charset,
		is_download,
		download_filename
	);
	if (
		send_t(np, resp, strlen(resp), 0) == SOCKET_ERROR ||
		( text_body_str_could_be_NULL ? (send_t(np, text_body_str_could_be_NULL, strlen(text_body_str_could_be_NULL), 0) == SOCKET_ERROR) : 0 )
		) {
		return -1;
	}
	else {
		return 0;
	}
}

int send_file(tcp_node* np, const char* filename, int keep_alive, const char* MIME_type, const char* file_charset, int is_download, const char* download_filename
	, const char* phrase_200
	, const char* html_200
	, const char* phrase_404
	, const char* html_404
	, const char* phrase_500
	, const char* html_500
) {
	char resp[5000];
	HANDLE hFile = get_file_hd(filename, 1).handle;
	if (hFile == NULL) {
		LogMe.et("send_file() [socket = %p ] [file = \"%s\" ] could not get file handle", np->socket, filename);
		return send_text(
			np,
			404,
			phrase_404,
			keep_alive,
			html_404,
			MIME_TYPE_HTML,
			HTTP_CHARSET_UTF8,
			0, NULL
		) == 0 ? 1 : -1;
	} else {
		LARGE_INTEGER fSize;
		if (GetFileSizeEx(hFile, &fSize) == 0) {
			CloseHandle(hFile); hFile = NULL;
			LogMe.et("send_file() [socket = %p ] [file = \"%s\" ] could not get the file size due to error: %lu", np->socket, filename, GetLastError());
			return send_text(
				np,
				500,
				phrase_500,
				keep_alive,
				html_500,
				MIME_TYPE_HTML,
				HTTP_CHARSET_UTF8,
				0, NULL
			) == 0 ? 2 : -1;
		} else {
			LogMe.it("sendind file [ \"%s\" ] <Size: %lld> to socket [ %p ] ...", filename, fSize.QuadPart, np->socket);
			http_response(
				resp,
				sizeof(resp),
				200,
				phrase_200,
				keep_alive,
				NULL,
				fSize.QuadPart,
				MIME_type,
				file_charset,
				is_download,
				download_filename
			);
			if (
				send_t(np, resp, strlen(resp), 0) == SOCKET_ERROR ||
				transmit_file(np, hFile, fSize.QuadPart, filename) != 0
				) {
				CloseHandle(hFile); hFile = NULL;
				return -1;
			}
			else {
				CloseHandle(hFile); hFile = NULL;
				return 0;
			}
		}
	}
}

int receive_file(tcp_node* np, const char* file_dir, const char* filename, int keep_alive, long long file_size
	, const char* phrase_200
	, const char* html_200
	, const char* phrase_500
	, const char* html_500
) {
	size_t fdstrlen = strlen(file_dir);
	size_t fnstrlen = strlen(filename);
	if (fdstrlen <= 0 || fnstrlen <= 0)
	{
		handle_open_fail:
		LogMe.et("receive_file() [socket = %p ] [file = \"%s\" ] could not get file handle", np->socket, filename);
		handle_500:
		return send_text(
			np,
			500,
			phrase_500,
			keep_alive,
			html_500,
			MIME_TYPE_HTML,
			HTTP_CHARSET_UTF8,
			0, NULL
		) == 0 ? 1 : -1;
	}
	if (file_size <= 0)
	{
		LogMe.et("receive_file() [socket = %p ] [file = \"%s\" ] file_size = %lld , error!", np->socket, filename, file_size);
		goto handle_500;
	}
	if (file_dir[fdstrlen-1] != '\\' || filename[0] == '\\' || filename[0] == '/')
	{
		goto handle_open_fail;
	}
	if (str_contain_relative_path(file_dir) || str_contain_relative_path(filename))
	{
		goto handle_open_fail;
	}
	char* combined_path = zero_malloc(fdstrlen+fnstrlen+1);
	if (!combined_path)
	{
		goto handle_open_fail;
	}
#ifdef V_WINDOWS
	/* system() may be unstable sometimes */
	/*const char* mkdir = "mkdir ";
	const char* mkdir_end = " >nul 2>&1";
	char* mkdir_cmd = zero_malloc(strlen(mkdir)+fdstrlen+strlen(mkdir_end)+1);
	if (!mkdir_cmd)
	{
		free(combined_path); combined_path = NULL;
		goto handle_open_fail;
	}
	strcat(mkdir_cmd, mkdir);
	strcat(mkdir_cmd, file_dir);
	strcat(mkdir_cmd, mkdir_end);
	system(mkdir_cmd);
	free(mkdir_cmd); mkdir_cmd = NULL;*/
#endif // V_WINDOWS
	strcat(combined_path, file_dir);
	strcat(combined_path, filename);
	HANDLE hFile = get_file_hd(combined_path, 0).handle;
	free(combined_path); combined_path = NULL;
	if (hFile == NULL) {
		goto handle_open_fail;
	}
	for (long long i = 0; i < file_size;) {
		char x[512000];
		long long rlen = file_size - i > sizeof(x) ? sizeof(x) : file_size - i;
		long long rtotal = 0;
		while (rtotal < rlen)
		{
			int rres = recv_t(np, x+rtotal, rlen - rtotal, 0);
			if (rres <= 0)
			{
				CloseHandle(hFile); hFile = NULL;
				return -1;
			}
			rtotal += rres;
		}
		long long bWritten = 0;
		BOOL wres = WriteFile(hFile, &x, rlen, &bWritten, NULL);
		if (!wres || bWritten < rlen) {
			LogMe.et("receive_file() [socket = %p ] [file = \"%s\" ] failed to write to file with error %lu", np->socket, filename, GetLastError());
			CloseHandle(hFile); hFile = NULL;
			goto handle_500;
		}
		//LogMe.bt("successfully write %lld bytes to file[ \"%s\" ]", rlen, filename);
		i += rlen;
	}
	CloseHandle(hFile); hFile = NULL;
	LogMe.it("receive_file() [socket = %p ] [file = \"%s\" ] completed with file_size = %lld", np->socket, filename, file_size);
	return send_text(
		np,
		200,
		phrase_200,
		keep_alive,
		html_200,
		MIME_TYPE_HTML,
		HTTP_CHARSET_UTF8,
		0, NULL
	) == 0 ? 0 : -1;
}

typedef struct generator_params {
	node* np;
	int recv_t_return_val;
} generator_params;

static char generator(void* params_p, int* continue_flag_p) {
	generator_params* gpp = params_p;
	node* np = gpp->np;
	char buf[1];
	gpp->recv_t_return_val = recv_t(np, buf, sizeof(buf), 0);
	*continue_flag_p = (gpp->recv_t_return_val == sizeof(buf));
	return buf[0];
}

static int printHttpHeader(vlist this_vlist, long i, void* extra) {
	const HttpHeader* header = this_vlist->get_const(this_vlist, i);
	LogMe.n("%s: %s", header->field, header->value);
	return 0; // go on
}

typedef struct max_contains {
	size_t pattern_str_len;
	long index;
	const char* path;
} max_contains;

static int testPattern(vlist this_vlist, long i, void* extra) {
	max_contains* mp = extra;
	HttpHandler* hhandler = this_vlist->get(this_vlist, i);
	if (hhandler->path_contains && strlen(hhandler->path_contains)>0 && strstr(mp->path, hhandler->path_contains) == mp->path) // "starts with"
	{
		size_t pattern_str_len = strlen(hhandler->path_contains);
		if (mp->index < 0 || pattern_str_len >= mp->pattern_str_len)
		{
			mp->index = i; mp->pattern_str_len = pattern_str_len;
		}
	}
	return 0; // go on
}

static DWORD WINAPI connection_run(_In_ LPVOID params_p) {
	params* pp = params_p;
	node* np = pp->node_p;
	generator_params gp = { .np = np };
	vlist http_handlers = pp->http_handlers;

	while (1)
	{
		char* message = NULL;
		HttpMethod method = INVALID_METHOD;
		int nres = next_http_message(&method, &message, generator, &gp, 0);
		LogMe.et("[ HTTP next_http_message() Res From Socket %p ] %d", np->socket, nres);
		if (nres >= 0)
		{
			LogMe.it("[ HTTP Message From Socket %p ] %s", np->socket, message);
			HttpMessage hmsg = parse_http_message(message, 0);
			if (!(hmsg.malloc_success))
			{
				LogMe.et("[ Parsed HTTP Message From Socket %p ] <Malloc Fail>", np->socket);
				freeHttpMessage(&hmsg);
				// response 500 then go on
				if (
					send_text(
						np,
						500,
						pp->phrase_500,
						1,
						pp->html_500,
						MIME_TYPE_HTML,
						HTTP_CHARSET_UTF8,
						0,
						NULL
					) != 0
					)
				{
					free(message); message = NULL;
					return error_shutdown(np, params_p, 6);
				}
			}
			else
			{
				if (!(hmsg.success))
				{
					LogMe.et("[ Parsed HTTP Message From Socket %p ] <Parse Fail> <%s><%s>", np->socket, hmsg.error_name, hmsg.error_reason);
					freeHttpMessage(&hmsg);
					// response 400 then go on
					if (
						send_text(
							np,
							400,
							pp->phrase_400,
							1,
							pp->html_400,
							MIME_TYPE_HTML,
							HTTP_CHARSET_UTF8,
							0,
							NULL
						) != 0
						)
					{
						free(message); message = NULL;
						return error_shutdown(np, params_p, 7);
					}
				}
				else
				{
					LogMe.bt("[ Parsed HTTP Message From Socket %p ] HTTP/%d.%d %s %s %lld", np->socket, hmsg.http_major, hmsg.http_minor, hmsg.url, getConstHttpMethodNameStr(hmsg.method), hmsg.content_length);
					LogMe.w("query string list:");
					if (hmsg.query_string)
						hmsg.query_string->foreach(hmsg.query_string, printHttpHeader, NULL);
					LogMe.w("fragment list:");
					if (hmsg.url_fragment)
						hmsg.url_fragment->foreach(hmsg.url_fragment, printHttpHeader, NULL);
					LogMe.w("HTTP header list:");
					if (hmsg.http_headers)
						hmsg.http_headers->foreach(hmsg.http_headers, printHttpHeader, NULL);
					int handled = 0;
					int handled_error = 1;
					if (http_handlers)
					{
						max_contains mc = {
							.index = -1,
							.pattern_str_len = -1,
							.path = hmsg.path
						};
						http_handlers->foreach(http_handlers, testPattern, &mc);
						if (mc.index >= 0)
						{
							handled = 1;
							HttpHandler* hdr = http_handlers->get(http_handlers, mc.index);
							HttpHandlerPac hpac = {
								.extra = hdr->extra,
								.node = np
							};
							handled_error = ((HTTP_HANDLE_FUNC_TYPE*)hdr->handle_func)(&hmsg, &hpac);
							freeHttpMessage(&hmsg);
							free(message); message = NULL;
						}
					}
					if (!handled)
					{
						long long content_length_f = hmsg.content_length;
						long long content_length = hmsg.content_length;
						freeHttpMessage(&hmsg);
						long recved_content_length = 0;
						char content[5096] = { 0 };
						// recv content
						while (content_length > 0)
						{
							long long r_len = content_length > 1024 ? 1024 : content_length;
							content_length -= r_len;
							char temp[1024] = { 0 };
							int r_res = recv_t(np, temp, r_len, 0);
							if (r_res == 0) {
								// recv 0 this time
								break;
							}
							else if (r_res == r_len)
							{
								if (recved_content_length + r_res < 5096)
								{
									memcpy(content + recved_content_length, temp, r_res);
								}
								recved_content_length += r_res;
							}
							else if (r_res > 0)
							{
								// recv a part this time
								if (recved_content_length + r_res < 5096)
								{
									memcpy(content + recved_content_length, temp, r_res);
								}
								recved_content_length += r_res;
								break;
							}
							else {
								free(message); message = NULL;
								return error_shutdown(np, params_p, 14);
							}
						}
						if (content_length_f > 0)
						{
							LogMe.it("[ HTTP Content From Socket %p ] length = %lld | received length = %ld", np->socket, content_length_f, recved_content_length);
							LogMe.n("%s", content);
						}
						// response 200 then go on
						if (
							send_text(
								np,
								200,
								pp->phrase_200,
								1,
								pp->html_200,
								MIME_TYPE_HTML,
								HTTP_CHARSET_UTF8,
								0,
								NULL
							) != 0
							)
						{
							free(message); message = NULL;
							return error_shutdown(np, params_p, 8);
						}
					}
					else
					{
						if (handled_error < 0)
						{
							return error_shutdown(np, params_p, handled_error);
						}
						else if (handled_error == 0)
						{
							return recv_0_shutdown(np, params_p, 999);
						}
						else if (handled_error == INT_MAX)
						{
							return active_shutdown(np, params_p, 9999);
						}
					}
				}
			}
		}
		else if (nres == -1)
		{
			if (
				send_text(
					np,
					500,
					pp->phrase_500,
					1,
					pp->html_500,
					MIME_TYPE_HTML,
					HTTP_CHARSET_UTF8,
					0,
					NULL
				) != 0
				)
			{
				return error_shutdown(np, params_p, 9);
			}
		}
		else if (nres == -2)
		{
			return error_shutdown(np, params_p, 10);
		}
		else if (nres == -3)
		{
			if (gp.recv_t_return_val > 0)
			{
				// 接收了一部分但出错或对方关闭了连接
				return recv_0_shutdown(np, params_p, 15);
			}
			else if (gp.recv_t_return_val == 0)
			{
				// 对方关闭了连接
				return recv_0_shutdown(np, params_p, 16);
			}
			else {
				// 出错了
				return error_shutdown(np, params_p, 17);
			}
			return error_shutdown(np, params_p, 11);
		}
		else if (nres == -4)
		{
			return error_shutdown(np, params_p, 12);
		}
		else
		{
			return error_shutdown(np, params_p, 13);
		}
		free(message); message = NULL;
	}

	//size_t read_num = 0;
	//char req[51] = { 0 };
	//int find_res = find_sub_str(50, generator, &gp, NULL, "abcdefg", &read_num, req, sizeof(req));
	//if (find_res >= 0)
	//{
	//	// found
	//	LogMe.it("[ %p ] %s", np->socket, req);
	//	const char* origin_name = "hello_world_你好——世界.jpg";
	//	char encoded_name[500];
	//	url_encode(origin_name, strlen(origin_name), encoded_name, sizeof(encoded_name), 0);
	//	if (
	//		send_file(
	//			np,
	//			"D:\\同步盘\\Pictures\\0 (手机).jpg",
	//			0,
	//			MIME_TYPE_JPEG,
	//			NULL,
	//			1,
	//			encoded_name
	//		) < 0
	//		) {
	//		LogMe.et("send_file() on socket [ %p ] failed", np->socket);
	//		return error_shutdown(np, params_p, 6);
	//	}
	//	return active_shutdown(np, params_p, 0);
	//}
	//else if (find_res == -1)
	//{
	//	// not found and no error
	//	if (
	//		send_text(
	//			np,
	//			404,
	//			,
	//			0,
	//			"<html>\n<body>\n<h1>File Not Found 文件找不到</h1>\n</body>\n</html>\n",
	//			MIME_TYPE_HTML,
	//			HTTP_CHARSET_UTF8,
	//			0,
	//			NULL
	//		) != 0
	//		)
	//	{
	//		return error_shutdown(np, params_p, 8);
	//	}
	//	return active_shutdown(np, params_p, 7);
	//}
	//else if (find_res == -2)
	//{
	//	// malloc fail
	//	LogMe.et("malloc fail on socket [ %p ] when parsing HTTP request", np->socket);
	//	return active_shutdown(np, params_p, 3);
	//}
	//else if (find_res == -3)
	//{
	//	// generator fail
	//	if (gp.recv_t_return_val == 0)
	//	{
	//		// recv 0
	//		LogMe.bt("recv_0 on socket [ %p ] when parsing HTTP request", np->socket);
	//		return recv_0_shutdown(np, params_p, 4);
	//	}
	//	else
	//	{
	//		// recv error
	//		LogMe.et("recv_t() on socket [ %p ] failed when parsing HTTP request with error: %d", np->socket, WSAGetLastError());
	//		return error_shutdown(np, params_p, 5);
	//	}
	//}
	return active_shutdown(np, params_p, -1); // 主动关闭连接
}

// return zero to remove current node from vlist
static int closed_cnt_filter(vlist this, long i, void* extra) {
	return ((node*)(this->get_const(this, i)))->open;
}

static void print_addrinfo_list(struct addrinfo *result) {
	int num = 0;
	for (;result != NULL; result = result->ai_next, num++)
	{
		char host_name[100];
		char port_str[100];
		getnameinfo(
			result->ai_addr, result->ai_addrlen, host_name, sizeof(host_name), port_str, sizeof(port_str), NI_NUMERICHOST | NI_NUMERICSERV
		);
		LogMe.it("addrinfo%d ip: [ %s ] port %s", num, host_name, port_str);
	}
}

void tcp_server_run(int port, int memmory_lack, vlist http_handlers
	, const char* phrase_200
	, const char* html_200
	, const char* phrase_400
	, const char* html_400
	, const char* phrase_500
	, const char* html_500
) {
	const char* exit_words = "tcp server exited";
	const char* hello_words = "tcp server started";
	const long max_cnt_list_size = memmory_lack ? 6 : 2000;

	LogMe.it(hello_words);

	if (init_winsock() != 0) {
		LogMe.et(exit_words);
		return;
	}

	struct addrinfo* result = NULL, hints;

	ZeroMemory(&hints, sizeof(hints));

	hints.ai_family = AF_INET6; // IPv6 for dual-stack socket
	hints.ai_socktype = SOCK_STREAM; // Stream socket
	hints.ai_protocol = IPPROTO_TCP; // Socket over TCP
	hints.ai_flags = AI_PASSIVE;  // Listen socket

	// 获取 socket 指示信息列表
	int iResult = getaddrinfo(
		NULL, // Listen socket
		vitoa(port, (char[30]){0}, 30), // Port
		&hints, // Hints
		&result // Results
	);

	if (iResult != 0) {
		LogMe.et("getaddrinfo failed: %d", iResult);
		WSACleanup();
		LogMe.et(exit_words);
		return;
	}

	// 打印指示信息列表
	print_addrinfo_list(result);
	
	// 根据获取到的第一个指示信息创建 监听套接字
	SOCKET ListenSocket = INVALID_SOCKET;
	ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);

	if (ListenSocket == INVALID_SOCKET) {
		LogMe.et("Error at socket(): %ld", WSAGetLastError());
		freeaddrinfo(result);
		WSACleanup();
		LogMe.et(exit_words);
		return;
	}

	// 设置套接字为 IPv4 IPv6 双协议栈套接字
	iResult = setsockopt(ListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&((DWORD) { 0 }), sizeof(DWORD));
	if (iResult == SOCKET_ERROR) {
		LogMe.et("setsockopt for IPV6_V6ONLY failed with error: %u", WSAGetLastError());
		closesocket(ListenSocket);
		freeaddrinfo(result);
		WSACleanup();
		LogMe.et(exit_words);
		return;
	}
	else {
		LogMe.it("Set IPV6_V6ONLY: false");
	}

	// 根据获取到的第一个指示信息绑定 监听套接字
	iResult = bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen);
	if (iResult == SOCKET_ERROR) {
		LogMe.et("bind failed with error: %d", WSAGetLastError());
		closesocket(ListenSocket);
		freeaddrinfo(result);
		WSACleanup();
		LogMe.et(exit_words);
		return;
	}
	LogMe.it("bind succeeded on port: %d", port);

	// 释放不再需要的指示信息列表
	freeaddrinfo(result);

	// 监听 监听套接字
	// SOMAXCONN 是 “接收新 TCP 连接队列” 的系统默认的最大长度
	if (listen(ListenSocket, SOMAXCONN) == SOCKET_ERROR) {
		LogMe.et("Listen failed with error: %ld", WSAGetLastError());
		closesocket(ListenSocket);
		WSACleanup();
		LogMe.et(exit_words);
		return;
	}
	LogMe.it("listen succeeded on port: %d", port);

	LogMe.it("listening...");

	tcp_server server = {
		// 初始化线程列表
		.connections_list = make_vlist(sizeof(node)),
		.has_open = 0
	};

	if (server.connections_list == NULL) {
		LogMe.et("Unable to init connections_list");
		closesocket(ListenSocket);
		WSACleanup();
		LogMe.et(exit_words);
		return;
	}

	SOCKET ClientSocket;
	struct sockaddr_storage client_sockaddr;
	int client_sockaddr_len;
	enum reasons { AcceptFail = 0, MallocFail, CreateThreadFail, SetSockOptFail, Debug } reason;
	SOCKET fail_socket = INVALID_SOCKET;

	// 接受新的 TCP 连接
	while (
		client_sockaddr_len = sizeof(client_sockaddr),
		ClientSocket = accept(ListenSocket, &client_sockaddr, &client_sockaddr_len),
		reason = (ClientSocket != INVALID_SOCKET)
		) {
		LogMe.it("accepted client socket: %p", ClientSocket);
		// 设置套接字为连接成功后调用 closesocket() 时立即释放读写资源、然后立即释放 socket 并返回，即 SO_DONTLINGER 设为 false
		iResult = setsockopt(ClientSocket, SOL_SOCKET, SO_DONTLINGER, (char*)&((DWORD) { 0 }), sizeof(DWORD));
		if (iResult == SOCKET_ERROR) {
			LogMe.et("setsockopt for client socket [ %p ] SO_DONTLINGER failed with error: %u", WSAGetLastError());
			fail_socket = ClientSocket;
			closesocket(ClientSocket); ClientSocket = INVALID_SOCKET;
			reason = SetSockOptFail;
			break;
		}
		node* np = malloc(sizeof(node));
		params* pp = malloc(sizeof(params));
		if (np == NULL || pp == NULL)
		{
			fail_socket = ClientSocket;
			closesocket(ClientSocket); ClientSocket = INVALID_SOCKET;
			free(np); np = NULL;
			free(pp); pp = NULL;
			reason = MallocFail;
			break;
		}
		np->handle = CreateThread(
			NULL, // 默认安全属性
			0, // 使用默认的栈初始物理内存大小（commit size）
			connection_run, // 指向线程起始函数的指针
			pp, // 要向线程起始函数传递的指针（此指针作为参数）
			CREATE_SUSPENDED, // 创建线程，但不马上运行
			&(np->tid) // 存放创建的线程的 ID 的地址
		);
		if (np->handle == NULL)
		{
			fail_socket = ClientSocket;
			closesocket(ClientSocket); ClientSocket = INVALID_SOCKET;
			free(np); np = NULL;
			free(pp); pp = NULL;
			reason = CreateThreadFail;
			break;
		}
		np->socket = ClientSocket;
		np->recv_timeout_s = DEFAULT_RECV_TIMEOUT_S;
		np->send_timeout_s = DEFAULT_SEND_TIMEOUT_S;
		np->open = 1;
		pp->node_p = np;
		pp->http_handlers = http_handlers;
		pp->phrase_200 = phrase_200;
		pp->html_200 = html_200;
		pp->phrase_400 = phrase_400;
		pp->html_400 = html_400;
		pp->phrase_500 = phrase_500;
		pp->html_500 = html_500;
		server.connections_list->quick_add(server.connections_list, np);
		LogMe.wt("Connection thread [tid = %lu ] [client socket = %p ] start.", np->tid, np->socket);
		ResumeThread(np->handle);

		LogMe.bt("connections_list Size: %ld", server.connections_list->size);

		if (server.connections_list->size > max_cnt_list_size)
		{
			LogMe.et("Removed %ld closed connections from connections_list", server.connections_list->flush(server.connections_list, closed_cnt_filter, NULL));
		}

		/* Debug Code */
		/*fail_socket = ClientSocket;
		reason = Debug;
		break;*/
	}

	// 解释退出原因
	switch (reason)
	{
	case AcceptFail:
		LogMe.et("Accept failed wiht error: %d", WSAGetLastError());
		break;
	case MallocFail:
		LogMe.et("[on accept socket %p ] Malloc failed", fail_socket);
		break;
	case CreateThreadFail:
		LogMe.et("[on accept socket %p ] CreateThread failed", fail_socket);
		break;
	case Debug:
		LogMe.et("[on accept socket %p ] Debug", fail_socket);
		break;
	default:
		LogMe.et("[on accept socket %p ] Unknown Error", fail_socket);
		break;
	}

	// 等待所有的连接线程都自动退出
	LogMe.et("waiting for all the connection threads to exit...");
	while (!all_closed(&server));
	LogMe.et("All the connection threads have exited");

	// 释放线程列表
	delete_vlist(server.connections_list, &(server.connections_list));

	closesocket(ListenSocket);
	WSACleanup();
	LogMe.et(exit_words);
}


#ifdef __cplusplus
}
#endif
