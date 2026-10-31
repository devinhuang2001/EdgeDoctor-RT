/* Persist the station credentials that this BSP's Wi-Fi manager replays at boot.
 *
 * The stock `wifi join` shell command only associates for the current power
 * cycle. The BSP's webnet_wifi.c writes /flash/wifi_config.json when a network
 * is configured through its HTTP portal, and nothing else populates that file,
 * so a board provisioned over the serial console silently returns to AP mode
 * after every reboot. This command closes that gap from the shell. */
#include <rtthread.h>

#include <dfs_posix.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define ED_WIFI_CONFIG_PATH "/flash/wifi_config.json"
#define ED_WIFI_SSID_MAX 64U
#define ED_WIFI_PASSWORD_MAX 64U

static int ed_wifi_write_config(const char *ssid, const char *password)
{
    char json[ED_WIFI_SSID_MAX + ED_WIFI_PASSWORD_MAX + 32U];
    int fd;
    int length;

    if (!ssid || !password || ssid[0] == '\0' ||
        strlen(ssid) >= ED_WIFI_SSID_MAX ||
        strlen(password) >= ED_WIFI_PASSWORD_MAX)
        return -1;
    /* The value is written verbatim into JSON and parsed back by cJSON at the
     * next boot, so a quote or backslash would corrupt the document. */
    if (strpbrk(ssid, "\"\\") != RT_NULL ||
        strpbrk(password, "\"\\") != RT_NULL)
        return -1;

    length = rt_snprintf(json, sizeof(json),
                         "{\"ssid\":\"%s\",\"password\":\"%s\"}", ssid, password);
    if (length <= 0 || (size_t)length >= sizeof(json))
        return -1;

    fd = open(ED_WIFI_CONFIG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0)
        return -1;
    if (write(fd, json, (size_t)length) != length)
    {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int edwifi(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "save") == 0)
    {
        if (ed_wifi_write_config(argv[2], argv[3]) != 0)
        {
            rt_kprintf("edwifi: failed to write %s\n", ED_WIFI_CONFIG_PATH);
            return -1;
        }
        rt_kprintf("edwifi: saved %s; the board joins it on the next boot\n",
                   ED_WIFI_CONFIG_PATH);
        rt_kprintf("edwifi: use `wifi join` to associate in this session\n");
        return 0;
    }
    rt_kprintf("usage: edwifi save <ssid> <password>\n");
    return -1;
}
MSH_CMD_EXPORT(edwifi, persist station credentials for boot-time auto connect);
