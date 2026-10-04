"""Enable modem-less PTYs in the isolated libserialport 0.1.1 test build.

No changes to protocol bytes or real serial-port behavior. PTYs have no modem
control lines; their unsupported TIOCM* requests are treated as deasserted.
"""
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
source = path.read_text()
pattern = r'ioctl\(port->fd, (?:TIOCMGET|config->(?:rts|dtr) == SP_(?:RTS|DTR)_ON \? TIOCMBIS : TIOCMBIC),\s*&(?:data->controlbits|controlbits|bits)\)'
source, count = re.subn(pattern, lambda m: m[0].replace('ioctl(', 'pty_modem_ioctl(', 1), source)
if count != 6:
    raise SystemExit(f'Unexpected libserialport source: {count} modem calls')
helper = '''
static int pty_modem_ioctl(int fd, unsigned long request, int *bits)
{
    int result = ioctl(fd, request, bits);
    if (result < 0 && errno == ENOTTY) {
        char name[128];
        if (ttyname_r(fd, name, sizeof(name)) == 0 &&
                strncmp(name, "/dev/pts/", 9) == 0) {
            if (request == TIOCMGET) *bits = 0;
            return 0;
        }
    }
    return result;
}

'''
marker = 'static enum sp_return get_config('
assert marker in source
path.write_text(source.replace(marker, helper + marker, 1))
