#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sqlite3.h>

#define SERVER_PORT 8080
#define BACKLOG 16
#define RECV_BUF_SIZE 16384
#define SMALL_BUF 1024
#define HTML_BUF_SIZE 131072
#define ROWS_BUF_SIZE 98304

static void html_escape_append(char *dst, size_t dst_sz, const char *src) {
    size_t used = strlen(dst);
    if (!src) return;

    while (*src && used + 8 < dst_sz) {
        const char *rep = NULL;
        switch (*src) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }

        if (rep) {
            size_t n = strlen(rep);
            if (used + n >= dst_sz) break;
            memcpy(dst + used, rep, n);
            used += n;
        } else {
            dst[used++] = *src;
        }
        src++;
    }
    dst[used] = '\0';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    size_t i = 0;

    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    while (src[i] && j + 1 < dst_sz) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            int hi = hexval(src[i + 1]);
            int lo = hexval(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[j++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[j++] = src[i];
            }
        } else {
            dst[j++] = src[i];
        }
        i++;
    }
    dst[j] = '\0';
}

static void get_query_param(const char *query, const char *key, char *out, size_t out_sz) {
    size_t key_len;
    const char *p;

    if (!out || out_sz == 0) return;
    out[0] = '\0';

    if (!query || !key) return;

    key_len = strlen(key);
    p = query;

    while (*p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);

        if (len > key_len + 1 && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            char encoded[SMALL_BUF];
            size_t vlen = len - key_len - 1;
            if (vlen >= sizeof(encoded)) vlen = sizeof(encoded) - 1;
            memcpy(encoded, p + key_len + 1, vlen);
            encoded[vlen] = '\0';
            url_decode(out, out_sz, encoded);
            return;
        }

        if (!amp) break;
        p = amp + 1;
    }
}

static int socket_printf(SOCKET sock, const char *fmt, ...) {
    char buffer[16384];
    int len;
    va_list args;

    va_start(args, fmt);
    len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len < 0) return -1;
    if (len >= (int)sizeof(buffer)) len = (int)sizeof(buffer) - 1;

    return send(sock, buffer, len, 0);
}

static void send_response(SOCKET client, const char *status, const char *content_type, const char *body) {
    size_t body_len = body ? strlen(body) : 0;
    if (!body) body = "";

    socket_printf(
        client,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Connection: close\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n"
        "%s",
        status,
        content_type,
        body_len,
        body
    );
}

static int exec_sql(sqlite3 *db, const char *sql) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", errmsg ? errmsg : "unknown error");
        sqlite3_free(errmsg);
        return 0;
    }
    return 1;
}

static void init_database(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    const char *schema =
        "CREATE TABLE IF NOT EXISTS devices ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "category TEXT NOT NULL,"
        "manufacturer TEXT NOT NULL,"
        "model TEXT,"
        "voltage TEXT,"
        "current_amp REAL,"
        "power_watts INTEGER,"
        "stock INTEGER DEFAULT 0,"
        "location TEXT,"
        "notes TEXT"
        ");";

    if (!exec_sql(db, schema)) return;

    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM devices", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare count statement: %s\n", sqlite3_errmsg(db));
        return;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        stmt = NULL;

        if (count == 0) {
            const char *seed =
                "INSERT INTO devices (name, category, manufacturer, model, voltage, current_amp, power_watts, stock, location, notes) VALUES"
                "('断路器 20A', '保护类', 'Schneider Electric', 'IC65N', '220V/380V', 20.0, 0, 30, 'A区-01', '小型断路器，适用于配电保护'),"
                "('中间继电器', '控制类', 'Omron', 'MY2N-J', '24V DC', 0.1, 3, 50, 'B区-08', '通用控制回路继电器'),"
                "('开关电源', '电源类', 'Mean Well', 'LRS-150-12', '220V AC 输入', 12.5, 150, 12, 'C区-03', '输出 12V DC，适合工控设备'),"
                "('温度传感器', '传感器', 'Honeywell', 'T775', '5V DC', 0.05, 1, 40, 'D区-05', '用于设备温度采集'),"
                "('指示灯', '显示类', 'Siemens', '3SB3', '24V DC', 0.08, 2, 80, 'E区-02', '面板安装型 LED 指示灯'),"
                "('接触器', '执行类', 'ABB', 'A9-30-10', '220V AC', 9.0, 2200, 16, 'A区-05', '适用于电机启停控制');";
            exec_sql(db, seed);
        }
    } else {
        fprintf(stderr, "Failed to step count statement: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
    }
}

static void append_device_rows(sqlite3 *db, const char *keyword, char *rows, size_t rows_sz) {
    sqlite3_stmt *stmt = NULL;
    char like_kw[SMALL_BUF];
    const char *sql =
        "SELECT id, name, category, manufacturer, model, voltage, current_amp, power_watts, stock, location, notes "
        "FROM devices "
        "WHERE (?1 = '' "
        "   OR name LIKE ?2 "
        "   OR category LIKE ?2 "
        "   OR manufacturer LIKE ?2 "
        "   OR model LIKE ?2 "
        "   OR location LIKE ?2 "
        "   OR notes LIKE ?2) "
        "ORDER BY id DESC;";

    rows[0] = '\0';
    snprintf(like_kw, sizeof(like_kw), "%%%s%%", keyword ? keyword : "");

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        snprintf(rows, rows_sz, "<tr><td colspan='11'>SQL 语句准备失败：%s</td></tr>", sqlite3_errmsg(db));
        return;
    }

    sqlite3_bind_text(stmt, 1, keyword ? keyword : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, like_kw, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        char id_buf[32];
        char current_buf[64];
        char power_buf[64];
        char stock_buf[64];
        char n_name[256] = "";
        char n_cat[128] = "";
        char n_manu[128] = "";
        char n_model[128] = "";
        char n_voltage[128] = "";
        char n_loc[128] = "";
        char n_notes[256] = "";
        char line[2048];
        double current_amp = sqlite3_column_double(stmt, 6);
        int power_watts = sqlite3_column_int(stmt, 7);
        int stock = sqlite3_column_int(stmt, 8);

        snprintf(id_buf, sizeof(id_buf), "%d", sqlite3_column_int(stmt, 0));
        snprintf(current_buf, sizeof(current_buf), "%.2f", current_amp);
        snprintf(power_buf, sizeof(power_buf), "%d", power_watts);
        snprintf(stock_buf, sizeof(stock_buf), "%d", stock);

        html_escape_append(n_name, sizeof(n_name), (const char *)sqlite3_column_text(stmt, 1));
        html_escape_append(n_cat, sizeof(n_cat), (const char *)sqlite3_column_text(stmt, 2));
        html_escape_append(n_manu, sizeof(n_manu), (const char *)sqlite3_column_text(stmt, 3));
        html_escape_append(n_model, sizeof(n_model), (const char *)(sqlite3_column_text(stmt, 4) ? sqlite3_column_text(stmt, 4) : (const unsigned char *)""));
        html_escape_append(n_voltage, sizeof(n_voltage), (const char *)(sqlite3_column_text(stmt, 5) ? sqlite3_column_text(stmt, 5) : (const unsigned char *)""));
        html_escape_append(n_loc, sizeof(n_loc), (const char *)(sqlite3_column_text(stmt, 9) ? sqlite3_column_text(stmt, 9) : (const unsigned char *)""));
        html_escape_append(n_notes, sizeof(n_notes), (const char *)(sqlite3_column_text(stmt, 10) ? sqlite3_column_text(stmt, 10) : (const unsigned char *)""));

        snprintf(
            line,
            sizeof(line),
            "<tr>"
            "<td>%s</td>"
            "<td>%s</td>"
            "<td>%s</td>"
            "<td>%s</td>"
            "<td>%s</td>"
            "<td>%s</td>"
            "<td>%s</td>"
            "<td>%s</td>"
            "<td>%s</td>"
            "<td>%s</td>"
            "<td>%s</td>"
            "</tr>",
            id_buf, n_name, n_cat, n_manu, n_model, n_voltage,
            current_buf, power_buf, stock_buf, n_loc, n_notes
        );

        if (strlen(rows) + strlen(line) + 1 < rows_sz) {
            strcat(rows, line);
        }
    }

    sqlite3_finalize(stmt);

    if (rows[0] == '\0') {
        snprintf(rows, rows_sz, "<tr><td colspan='11' style='text-align:center;color:#666;'>没有匹配的设备记录。</td></tr>");
    }
}

static void render_home_page(SOCKET client, sqlite3 *db, const char *keyword) {
    char html[HTML_BUF_SIZE];
    char rows[ROWS_BUF_SIZE];
    char safe_kw[SMALL_BUF];

    safe_kw[0] = '\0';
    html_escape_append(safe_kw, sizeof(safe_kw), keyword ? keyword : "");
    append_device_rows(db, keyword, rows, sizeof(rows));

    snprintf(
        html,
        sizeof(html),
        "<!DOCTYPE html>"
        "<html lang='zh-CN'>"
        "<head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>电气设备台账查询系统</title>"
        "<style>"
        "body{margin:0;padding:24px;font-family:'Microsoft YaHei',Arial,sans-serif;background:#f3f6fb;color:#1f2937;}"
        ".wrap{max-width:1360px;margin:0 auto;}"
        ".card{background:#fff;border-radius:16px;padding:24px;box-shadow:0 10px 30px rgba(0,0,0,.08);}"
        "h1{margin:0 0 8px;font-size:30px;}"
        "p.desc{margin:0 0 18px;color:#4b5563;line-height:1.7;}"
        ".meta{background:#eff6ff;border-left:4px solid #2563eb;padding:12px 14px;border-radius:8px;margin-bottom:18px;color:#1e3a8a;}"
        "form{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:18px;}"
        "input[type=text]{flex:1;min-width:260px;padding:12px 14px;border:1px solid #cbd5e1;border-radius:10px;font-size:15px;}"
        "button,.btn{padding:12px 18px;border:none;border-radius:10px;background:#2563eb;color:#fff;text-decoration:none;font-size:15px;cursor:pointer;}"
        ".btn{display:inline-block;}"
        ".table-wrap{overflow:auto;border:1px solid #e5e7eb;border-radius:12px;}"
        "table{width:100%;border-collapse:collapse;min-width:1180px;background:#fff;}"
        "thead th{background:#f8fafc;position:sticky;top:0;z-index:1;}"
        "th,td{padding:12px 10px;border-bottom:1px solid #e5e7eb;text-align:left;vertical-align:top;font-size:14px;}"
        "tbody tr:hover{background:#f9fbff;}"
        ".footer{margin-top:16px;color:#6b7280;font-size:13px;}"
        "code{background:#f3f4f6;padding:2px 6px;border-radius:6px;}"
        "</style>"
        "</head>"
        "<body>"
        "<div class='wrap'>"
        "<div class='card'>"
        "<h1>电气设备台账查询系统</h1>"
        "<p class='desc'>这是一个使用 C 语言编写、由 Tiny C Compiler 编译、基于 Winsock 提供 HTTP 服务、以 SQLite 作为本地数据库的轻量级查询系统。适用于小型仓库、电气元件管理、设备备件台账等场景。</p>"
        "<div class='meta'>访问路径：<code>/</code> 查看页面，<code>/health</code> 做健康检查；通过查询参数 <code>?q=关键字</code> 进行模糊检索。</div>"
        "<form method='GET' action='/'>"
        "<input type='text' name='q' value='%s' placeholder='请输入设备名称、类别、厂家、型号、位置或备注'>"
        "<button type='submit'>查询</button>"
        "<a class='btn' href='/'>重置</a>"
        "</form>"
        "<div class='table-wrap'>"
        "<table>"
        "<thead><tr>"
        "<th>ID</th><th>名称</th><th>类别</th><th>厂家</th><th>型号</th><th>电压</th><th>电流(A)</th><th>功率(W)</th><th>库存</th><th>位置</th><th>备注</th>"
        "</tr></thead>"
        "<tbody>%s</tbody>"
        "</table>"
        "</div>"
        "<div class='footer'>默认监听地址：<code>http://127.0.0.1:8080/</code>。首次运行时，程序会自动创建数据库文件及示例数据。</div>"
        "</div>"
        "</div>"
        "</body>"
        "</html>",
        safe_kw,
        rows
    );

    send_response(client, "200 OK", "text/html", html);
}

static void handle_client(SOCKET client, sqlite3 *db) {
    char recv_buf[RECV_BUF_SIZE + 1];
    int n;
    char method[16] = {0};
    char raw_url[2048] = {0};
    char path[2048] = {0};
    char *query = NULL;
    char keyword[SMALL_BUF];
    const char *p;

    n = recv(client, recv_buf, RECV_BUF_SIZE, 0);
    if (n <= 0) return;
    recv_buf[n] = '\0';

    if (sscanf(recv_buf, "%15s %2047s", method, raw_url) != 2) {
        send_response(client, "400 Bad Request", "text/plain", "Malformed request.\n");
        return;
    }

    if (strcmp(method, "GET") != 0) {
        send_response(client, "405 Method Not Allowed", "text/plain", "Only GET is supported.\n");
        return;
    }

    if (strncmp(raw_url, "http://", 7) == 0 || strncmp(raw_url, "https://", 8) == 0) {
        p = strstr(raw_url, "://");
        if (p) {
            p += 3;
            p = strchr(p, '/');
            if (p) {
                strncpy(path, p, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
            } else {
                strcpy(path, "/");
            }
        } else {
            strncpy(path, raw_url, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }
    } else {
        strncpy(path, raw_url, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    query = strchr(path, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    if (strcmp(path, "/health") == 0) {
        send_response(client, "200 OK", "text/plain", "ok\n");
        return;
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        get_query_param(query, "q", keyword, sizeof(keyword));
        render_home_page(client, db, keyword);
        return;
    }

    send_response(client, "404 Not Found", "text/plain", "Route not found.\n");
}

int main(int argc, char *argv[]) {
    WSADATA wsa;
    SOCKET server_sock = INVALID_SOCKET;
    sqlite3 *db = NULL;
    struct sockaddr_in server_addr;
    const char *db_path = (argc > 1) ? argv[1] : "devices.db";
    int opt = 1;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed.\n");
        return 1;
    }

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        WSACleanup();
        return 1;
    }

    init_database(db);

    server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        sqlite3_close(db);
        WSACleanup();
        return 1;
    }

    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) == SOCKET_ERROR) {
        fprintf(stderr, "setsockopt() failed: %d\n", WSAGetLastError());
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed: %d\n", WSAGetLastError());
        closesocket(server_sock);
        sqlite3_close(db);
        WSACleanup();
        return 1;
    }

    if (listen(server_sock, BACKLOG) == SOCKET_ERROR) {
        fprintf(stderr, "listen() failed: %d\n", WSAGetLastError());
        closesocket(server_sock);
        sqlite3_close(db);
        WSACleanup();
        return 1;
    }

    printf("Server started: http://127.0.0.1:%d/\n", SERVER_PORT);
    printf("Database file : %s\n", db_path);
    printf("Health check  : http://127.0.0.1:%d/health\n", SERVER_PORT);
    printf("Press Ctrl+C to stop.\n");

    while (1) {
        SOCKET client_sock;
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);

        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock == INVALID_SOCKET) {
            fprintf(stderr, "accept() failed: %d\n", WSAGetLastError());
            continue;
        }

        handle_client(client_sock, db);
        closesocket(client_sock);
    }

    closesocket(server_sock);
    sqlite3_close(db);
    WSACleanup();
    return 0;
}