#include <stdio.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_BAUDRATE 57600

#ifdef linux
#define MODEMDEVICE "/dev/ttyS0"
#else
#define MODEMDEVICE "/dev/com1"
#endif /* linux */

#define DEFAULT_DELAY 6000

#define SLIP_END     0300
#define SLIP_ESC     0333
#define SLIP_ESC_END 0334
#define SLIP_ESC_ESC 0335

static const unsigned char slipend = SLIP_END;

#define CSNA_INIT 0x01

#define BUFSIZE 40
#define HCOLS 20
#define ICOLS 18

enum {
  MODE_START_DATE,
  MODE_DATE,
  MODE_START_TEXT,
  MODE_TEXT,
  MODE_INT,
  MODE_HEX,
  MODE_SLIP_AUTO,
  MODE_SLIP,
  MODE_SLIP_HIDE
};

static unsigned char rxbuf[2048];

static int
usage(const char *prog, int result)
{
  printf("Usage: %s [options] [SERIALDEVICE]\n", prog);
  printf("  -B BAUDRATE (default 57600)\n");
  printf("  -x for hexadecimal output\n");
  printf("  -i for decimal output\n");
  printf("  -s for automatic SLIP mode\n");
  printf("  -so for SLIP only mode (all data is SLIP packets)\n");
  printf("  -sn to hide SLIP packages\n");
  printf("  -t to add time as msec for each text line\n");
  printf("  -t0 to add time since start (msec) for each text line\n");
  printf("  -T[FORMAT] to add time for each text line\n");
  printf("    (see man page for strftime() for format description)\n");
  printf("  -d DELAY  for delay in usec between 2 consecutive writes\n");
  return result;
}

static void
print_hex_line(char *prefix, unsigned char *outbuf, int index)
{
  int i;

  printf("\r%s", prefix);
  for(i = 0; i < index; i++) {
    if((i % 4) == 0) {
      printf(" ");
    }
    printf("%02X", outbuf[i]);
  }
  printf("  ");
  for(i = index; i < HCOLS; i++) {
    if((i % 4) == 0) {
      printf(" ");
    }
    printf("  ");
  }
  for(i = 0; i < index; i++) {
    if(outbuf[i] < 30 || outbuf[i] > 126) {
      printf(".");
    } else {
      printf("%c", outbuf[i]);
    }
  }
}

int main(int argc, char **argv)
{
  const char *prog;
  int c;
  int fd;
  struct termios options;
  fd_set mask, smask;
  unsigned int baudrate = DEFAULT_BAUDRATE;
  speed_t speed;
  char *device = MODEMDEVICE;
  char *timeformat = NULL;
  struct timeval start_tv;
  int starttime = 0;
  unsigned char buf[BUFSIZE];
  char outbuf[HCOLS];
  unsigned char mode = MODE_START_TEXT;
  int index, nfound, flags = 0;
  unsigned char lastc = '\0';
  int delay = DEFAULT_DELAY;

  prog = argv[0];
  while((c = getopt(argc, argv, "b:B:xis::t::T::d:h")) != -1) {
    switch(c) {
    case 'b':
    case 'B':
      baudrate = atoi(optarg);
      break;

    case 'x':
      mode = MODE_HEX;
      break;
    case 'i':
      mode = MODE_INT;
      break;
    case 's':
      mode = MODE_SLIP_AUTO;
      if(optarg) {
        switch(*optarg) {
        case 'n':
          mode = MODE_SLIP_HIDE;
          break;
        case 'o':
          mode = MODE_SLIP;
          break;
        }
      }
      break;
    case 't': {
      struct timezone tz;
      gettimeofday(&start_tv, &tz);
      timeformat = NULL;
      mode = MODE_START_DATE;
      if(optarg && *optarg == '0') {
        starttime = 1;
      }
      break;
    }
    case 'T':
      if(optarg) {
        timeformat = optarg;
      } else {
        timeformat = "%Y-%m-%d %H:%M:%S";
      }
      mode = MODE_START_DATE;
      break;
    case 'd':
      delay = atoi(optarg);
      if(delay < 0){
        fprintf(stderr, "Delay must not be negative\n");
        return usage(prog, 1);
      }
      break;
    case '?':
    case 'h':
      return usage(prog, 0);
    default:
      fprintf(stderr, "unknown option '%c'\n", c);
      return usage(prog, 1);
    }
  }
  argc -= optind - 1;
  argv += optind - 1;

  if(argc > 2) {
    fprintf(stderr, "Too many arguments\n");
    return usage(prog, 1);
  }
  if(argc == 2) {
    device = argv[1];
  }

  switch(baudrate) {
  case 9600:
    speed = B9600;
    break;
  case 19200:
    speed = B19200;
    break;
  case 38400:
    speed = B38400;
    break;
  case 57600:
    speed = B57600;
    break;
  case 115200:
    speed = B115200;
    break;
#ifdef B230400
  case 230400:
    speed = B230400;
    break;
#elif defined(__APPLE__)
  case 230400:
    speed = 230400;
    break;
#endif
#ifdef B460800
  case 460800:
    speed = B460800;
    break;
#elif defined(__APPLE__)
  case 460800:
    speed = 460800;
    break;
#endif
#ifdef B921600
  case 921600:
    speed = B921600;
    break;
#elif defined(__APPLE__)
  case 921600:
    speed = 921600;
    break;
#endif
  default:
    fprintf(stderr, "unknown baudrate %u\n", baudrate);
    return usage(prog, 1);
  }

  fprintf(stderr, "connecting to %s (%u)", device, baudrate);

  fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY | O_SYNC );
  if(fd <0) {
    fprintf(stderr, "\n");
    perror(device);
    exit(-1);
  }
  fprintf(stderr, " [OK]\n");

  if(fcntl(fd, F_SETFL, 0) < 0) {
    perror("could not set fcntl");
    exit(-1);
  }

  if(tcgetattr(fd, &options) < 0) {
    perror("could not get options");
    exit(-1);
  }
/*   fprintf(stderr, "serial options set\n"); */
  cfsetispeed(&options, speed);
  cfsetospeed(&options, speed);
  /* Enable the receiver and set local mode */
  options.c_cflag |= (CLOCAL | CREAD);
  /* Mask the character size bits and turn off (odd) parity */
  options.c_cflag &= ~(CSIZE|PARENB|PARODD);
  /* Select 8 data bits */
  options.c_cflag |= CS8;

  /* Raw input */
  options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
  /* Raw output */
  options.c_oflag &= ~OPOST;

  if(tcsetattr(fd, TCSANOW, &options) < 0) {
    perror("could not set options");
    exit(-1);
  }

  /* Make read() return immediately */
/*    if(fcntl(fd, F_SETFL, FNDELAY) < 0) { */
/*      perror("\ncould not set fcntl"); */
/*      exit(-1); */
/*    } */

  FD_ZERO(&mask);
  FD_SET(fd, &mask);
  FD_SET(fileno(stdin), &mask);

  index = 0;
  for(;;) {
    smask = mask;
    nfound = select(FD_SETSIZE, &smask, (fd_set *) 0, (fd_set *) 0,
		    (struct timeval *) 0);
    if(nfound < 0) {
      if(errno == EINTR) {
	fprintf(stderr, "interrupted system call\n");
	continue;
      }
      /* something is very wrong! */
      perror("select");
      exit(1);
    }

    if(FD_ISSET(fileno(stdin), &smask)) {
      /* data from standard in */
      int n = read(fileno(stdin), buf, sizeof(buf));
      if(n < 0) {
	perror("could not read");
	exit(-1);
      } else if(n > 0) {
        int i;
        /*	  fprintf(stderr, "SEND %d bytes\n", n);*/
        if(mode == MODE_SLIP) {
          write(fd, &slipend, 1);
        }
        /* write slowly */
        for(i = 0; i < n; i++) {
          if(write(fd, &buf[i], 1) <= 0) {
            perror("write");
            exit(1);
          } else {
            fflush(NULL);
            if(delay > 0) {
              usleep(delay);
            }
          }
        }
        if(mode == MODE_SLIP) {
          write(fd, &slipend, 1);
          fflush(NULL);
        }
      }
    }

    if(FD_ISSET(fd, &smask)) {
      int i, n = read(fd, buf, sizeof(buf));
      if(n < 0) {
	perror("could not read");
	exit(-1);
      }

      for(i = 0; i < n; i++) {
	switch(mode) {
	case MODE_START_TEXT:
	case MODE_TEXT:
	  printf("%c", buf[i]);
	  break;
	case MODE_START_DATE:
	  if(timeformat == NULL) {
	    struct timeval tv;

	    gettimeofday(&tv, NULL);

	    if(starttime) {
	      printf("%4lu.%03lu: ",
                     (unsigned long)(tv.tv_sec - start_tv.tv_sec),
                     (unsigned long)(tv.tv_usec / 1000));
	    } else {
	      printf("%8lu.%03lu: ",
                     (unsigned long)tv.tv_sec,
                     (unsigned long)(tv.tv_usec / 1000));
	    }

	  } else {
            time_t t;
            t = time(&t);
            strftime(outbuf, HCOLS, timeformat, localtime(&t));
            printf("%s|", outbuf);
          }
          mode = MODE_DATE;

	  /* continue into the MODE_DATE */
	case MODE_DATE:
	  printf("%c", buf[i]);
	  if(buf[i] == '\n') {
	    mode = MODE_START_DATE;
	  }
	  break;
	case MODE_INT:
	  printf("%03d ", buf[i]);
	  if(++index >= ICOLS) {
	    index = 0;
	    printf("\n");
	  }
	  break;
	case MODE_HEX:
	  rxbuf[index++] = buf[i];
	  if(index >= HCOLS) {
	    print_hex_line("", rxbuf, index);
	    index = 0;
	    printf("\n");
	  }
	  break;

	case MODE_SLIP_AUTO:
	case MODE_SLIP_HIDE:
	  if(!flags && (buf[i] != SLIP_END)) {
	    /* Not a SLIP packet? */
	    printf("%c", buf[i]);
	    break;
	  }
	  /* continue to slip only mode */
	case MODE_SLIP:
	  switch(buf[i]) {
	  case SLIP_ESC:
	    lastc = SLIP_ESC;
	    break;

	  case SLIP_END:
	    if(index > 0) {
	      if(flags != 2 && mode != MODE_SLIP_HIDE) {
		char *prefix = "SLIP:";
		int n = 0;
		/* not overflowed: show packet */
		do {
		  print_hex_line(prefix, &rxbuf[n],
				 (index - n) > HCOLS ? HCOLS : (index - n));
		  n += HCOLS;
		  putchar('\n');
		  prefix = "     ";
		} while(n < index);
	      }
	      lastc = '\0';
	      index = 0;
	      flags = 0;
	    } else {
	      flags = !flags;
	    }
	    break;

	  default:
	    if(lastc == SLIP_ESC) {
	      lastc = '\0';

	      /* Previous read byte was an escape byte, so this byte will be
		 interpreted differently from others. */
	      switch(buf[i]) {
	      case SLIP_ESC_END:
		buf[i] = SLIP_END;
		break;
	      case SLIP_ESC_ESC:
		buf[i] = SLIP_ESC;
		break;
	      }
	    }

	    rxbuf[index++] = buf[i];
	    if(index >= (int)sizeof(rxbuf)) {
	      fprintf(stderr, "**** slip overflow\n");
	      index = 0;
	      flags = 2;
	    }
	    break;
	  }
	  break;
	}
      }

      /* after processing for some output modes */
      if(index > 0) {
	switch(mode) {
	case MODE_HEX:
	  print_hex_line("", rxbuf, index);
	  break;
	}
      }
      fflush(stdout);
    }
  }
}
