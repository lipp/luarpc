// Serial inteface implementation for POSIX-compliant systems

#include "serial.h"
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <time.h>

// Open the serial port
ser_handler ser_open( const char* sername )
{
  int fd;

  if( ( fd = open( sername, O_RDWR | O_NOCTTY | O_NDELAY ) ) == -1 )
    perror( "ser_open: unable to open port" );
  else
    fcntl( fd, F_SETFL, 0 );
  
  return ( ser_handler )fd;
}

// Close the serial port
void ser_close( ser_handler id )
{
  close( ( int )id );
}

// Helper function: get baud ID from actual baud rate
#define BAUDCASE(x)  case x: return B##x
static uint32_t ser_baud_to_id( uint32_t baud )
{
  switch( baud )
  {
    BAUDCASE( 1200 );
    BAUDCASE( 1800 );
    BAUDCASE( 2400 );
    BAUDCASE( 4800 );
    BAUDCASE( 9600 );
    BAUDCASE( 19200 );
    BAUDCASE( 38400 );
    BAUDCASE( 57600 );
    BAUDCASE( 115200 );
    BAUDCASE( 230400 );
  }
  return 0;
}

// Helper function: get number of bits ID from actual number of bits
#define NBCASE(x) case x: return CS##x
static int ser_number_of_bits_to_id( int nb )
{
  switch( nb )
  {
    NBCASE( 5 );
    NBCASE( 6 );
    NBCASE( 7 );
    NBCASE( 8 );
  }
  return 0;
}

int ser_setup( ser_handler id, uint32_t baud, int databits, int parity, int stopbits )
{
  struct termios termdata;
  struct timespec tsleep;
  int hnd = ( int )id;

  tsleep.tv_sec  = 0;
  tsleep.tv_nsec = 200000000;
  nanosleep( &tsleep, NULL );
  tcgetattr( hnd, &termdata );

  // Baud rate
  cfsetispeed( &termdata, ser_baud_to_id( baud ) );
  cfsetospeed( &termdata, ser_baud_to_id( baud ) );

  // Parity / stop bits
  if ( stopbits == SER_STOPBITS_2)
  {
      termdata.c_cflag |= CSTOPB;
  }
  else
  {
    termdata.c_cflag &= ~CSTOPB;
  }
  
  if( parity == SER_PARITY_NONE ) // no parity
  {
    termdata.c_cflag &= ~PARENB;
  }
  else if( parity == SER_PARITY_EVEN ) // even parity
  {
    termdata.c_cflag |= PARENB;
    termdata.c_cflag &= ~PARODD;
  }
  else if( parity == SER_PARITY_ODD ) // odd parity
  {
    termdata.c_cflag |= PARENB;
    termdata.c_cflag |= PARODD;
  }

   // Data bits
  termdata.c_cflag |= ( CLOCAL | CREAD );
  termdata.c_cflag &= ~CSIZE;
  termdata.c_cflag |= ser_number_of_bits_to_id( databits );

  // Disable HW and SW flow control
#if defined( CRTSCTS ) // not available on all platforms, use if available
  termdata.c_cflag &= ~CRTSCTS;
#endif
  termdata.c_iflag &= ~( IXON | IXOFF | IXANY );

  // Raw input
  termdata.c_lflag &= ~( ICANON | ECHO | ECHOE | ISIG );

  // Raw output
  termdata.c_oflag &= ~OPOST;

  // Check and strip parity bit if needed
  if( parity == SER_PARITY_NONE )
    termdata.c_iflag &= ~( INPCK | ISTRIP );
  else
    termdata.c_iflag |= ( INPCK | ISTRIP );

  // Set the attibutes now
  tcsetattr( hnd, TCSANOW, &termdata );

  // Flush everything
  tcflush( hnd, TCIOFLUSH );

  // And set blocking mode by default
  fcntl( id, F_SETFL, 0 );
  
  return SER_OK;
}

// Read up to the specified number of bytes, return bytes actually read
uint32_t ser_read( ser_handler id, uint8_t* dest, uint32_t maxsize )
{
  return ( uint32_t )read( ( int )id, dest, maxsize );
}

// Read a single byte and return it (or -1 for error)
int ser_read_byte( ser_handler id )
{
  uint8_t data;
  int res = ser_read( id, &data, 1 );

  return res == 1 ? data : -1;
}

// Write up to the specified number of bytes, return bytes actually written
uint32_t ser_write( ser_handler id, const uint8_t *src, uint32_t size )
{
  uint32_t res;
  
  res = ( uint32_t )write( ( int )id, src, size );
  return res;
}

// Write a byte to the serial port
uint32_t ser_write_byte( ser_handler id, uint8_t data )
{
  return ( uint32_t )write( id, &data, 1 );
}

// Set communication timeout
void ser_set_timeout_ms( ser_handler id, uint32_t timeout )
{
  struct termios termdata;

  tcgetattr( id, &termdata );
  if( timeout == SER_INF_TIMEOUT )
  {
    termdata.c_cc[ VTIME ] = 0;
    fcntl( id, F_SETFL, 0 ); // calls block
  }
  else if( timeout == SER_NO_TIMEOUT)
  {
    termdata.c_cc[ VTIME ] = 0; 
    fcntl( id, F_SETFL, O_NDELAY ); // no blocking, timeout
  }
  else
  {
    termdata.c_cc[ VTIME ] = timeout / 100;
    fcntl( id, F_SETFL, 0 ); // calls block
  }
  tcsetattr( id, TCSANOW, &termdata );
}

// Check if data is available for reading
int ser_readable( ser_handler id )
{
  fd_set rdfs;
  int ret;
  struct timeval tv;
  
  FD_ZERO (&rdfs);
  FD_SET (id, &rdfs);
 
  /* Wait up to five seconds. */
  tv.tv_sec = 5;
  tv.tv_usec = 0;

  ret = select( id+1, &rdfs, NULL, NULL, &tv );
  
  if (ret < 0)
  {
    return ret;
  }

  return ( ret > 0 );
}
