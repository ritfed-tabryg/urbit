/* vere/khan.c
**
*/
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <uv.h>
#include <errno.h>

#include "all.h"
#include "vere/vere.h"

/* u3_khan: control plane socket
*/
  typedef struct _u3_khan {
    u3_auto           car_u;            //  driver
    uv_pipe_t         pyp_u;            //  socket
    c3_l              sev_l;            //  number (of instance)
  } u3_khan;

#define URB_SOCK_PATH ".urb/khan.sock"
#define SOCK_TEMPLATE "/tmp/urb-khan.XXXXXX"

/* _khan_conn_cb(): socket connection callback.
*/
static void
_khan_conn_cb(uv_stream_t* sem_u, c3_i tas_i)
{
  // TODO interact
  //
  // There are four cases to think about here:
  //
  // 1. peek, runtime
  // 2. poke, runtime
  // 3. peek, arvo
  // 4. poke, arvo
  //
  // For 1, the driver itself parses and responds to a few basic text-mode
  // commands:
  // `ok`:   driver responds with "ok\n" (basic health test)
  // `dump`: dumps runtime stat counters in simple machine/human-readable format
  //
  // There is not yet a use case for 2.
  //
  // For 3 and 4, we speak a binary protocol using jammed nouns, as (to be)
  // documented in pkg/arvo/sys/vane/khan.hoon.
  //
  // The transition to arvo is signalled by a 0x80 (i.e. byte with MSB high) at
  // the start of a line (i.e. as first character, or immediately following a
  // '\n'.) Everything after that is simply forwarded to arvo, and responses
  // forwarded back, for the rest of the duration of the connection.
  //
  // Some alternate protocol designs:
  //
  // a. Forward everything to arvo.
  // b. Add a command that sends a single-shot jammed noun, which receives a
  //    single-shot response.
  // c. Listen on two or more different sockets.
  //
  // (a) is undesirable since we don't want to sync the runtime stats with
  // arvo. Continuous sync would add unnecessary load, and on-demand sync would
  // add unnecessary implementation complexity in the form of extra round-trips
  // between arvo and the runtime. (b) is probably fine, but I'm not smart
  // enough to know how to tell from the runtime when a jammed noun has
  // finished sending without base64-encoding it or something. (c) is tedious;
  // the same effect can be achieved by just opening two connections to the
  // socket, keeping one in text mode, and sending a 0x80 over the other.
}

/* _khan_close_cb(): callback for uv_close; nop.
*/
static void
_khan_close_cb(uv_handle_t* had_u)
{
}

/* _khan_ef_form(): start socket listener.
*/
static void
_khan_ef_form(u3_khan* cop_u)
{
  c3_i sat_i;

  // XX The full socket path is limited to about 108 characters, and we want it
  // to be relative to the pier. So we make the socket in a temporary directory
  // known to be within the length limit, then move it to the pier directory.
  //
  // pad_c is the template passed to mkdtemp. paf_c is the socket path within
  // the temporary directory, passed to uv_pipe_bind. pax_c is the final socket
  // location within the pier.
  c3_c pad_c[] = SOCK_TEMPLATE;
  c3_c paf_c[sizeof(SOCK_TEMPLATE "/s")];
  c3_c pax_c[2048];
  c3_i fid_i;

  if ( NULL == mkdtemp(pad_c) ) {
    u3l_log("khan: mkdtemp: %s\n", uv_strerror(errno));
    u3_king_bail();
  }

  if ( sizeof(paf_c) != 1 + snprintf(paf_c, sizeof(paf_c), "%s/s", pad_c) ) {
    u3l_log("khan: snprintf: failed (pad_c: %s)\n", pad_c);
    unlink(pad_c);
    u3_king_bail();
  }

  if ( sizeof(pax_c) < snprintf(pax_c, sizeof(pax_c), "%s/%s", u3_Host.dir_c,
                                URB_SOCK_PATH) )
  {
    u3l_log("khan: snprintf: pier directory too long (max %zd, got %s)\n",
            sizeof(((struct sockaddr_un*)0)->sun_path), u3_Host.dir_c);
    rmdir(pad_c);
    u3_king_bail();
  }

  if ( (sat_i = uv_pipe_init(u3L, &cop_u->pyp_u, 0)) < 0 ) {
    u3l_log("khan: uv_pipe_init: %s\n", uv_strerror(sat_i));
    rmdir(pad_c);
    u3_king_bail();
  }

  if ( (sat_i = uv_pipe_bind(&cop_u->pyp_u, pax_c)) < 0 ) {
    u3l_log("khan: uv_pipe_bind: %s\n", uv_strerror(sat_i));
    rmdir(pad_c);
    u3_king_bail();
  }

  if ( (sat_i = uv_listen((uv_stream_t*)&cop_u->pyp_u, 0,
                          _khan_conn_cb)) < 0 ) {
    u3l_log("khan: uv_listen: %s\n", uv_strerror(sat_i));
    unlink(paf_c);
    rmdir(pad_c);
    uv_close((uv_handle_t*)&cop_u->pyp_u, _khan_close_cb);
    u3_king_bail();
  }

  if ( -1 == rename(paf_c, pax_c) ) {
    u3l_log("khan: rename: %s\n", uv_strerror(errno));
    unlink(paf_c);
    rmdir(pad_c);
    uv_close((uv_handle_t*)&cop_u->pyp_u, _khan_close_cb);
    u3_king_bail();
  }

  rmdir(pad_c);

  u3l_log("khan: socket open\n");
  cop_u->car_u.liv_o = c3y;
}

/* _khan_born_news(): initialization complete, open socket.
*/
static void
_khan_born_news(u3_ovum* egg_u, u3_ovum_news new_e)
{
  u3_auto* car_u = egg_u->car_u;
  u3_khan* cop_u = (u3_khan*)car_u;

  if ( u3_ovum_done == new_e ) {
    _khan_ef_form(cop_u);
  }
}

/* _khan_born_bail(): nonessential failure; log it and keep going.
*/
static void
_khan_born_bail(u3_ovum* egg_u, u3_noun lud)
{
  u3l_log("khan: %%born failure; socket not opened\n");
}

/* _khan_io_talk(): notify %khan that we're live
*/
static void
_khan_io_talk(u3_auto* car_u)
{
  u3_khan* cop_u = (u3_khan*)car_u;

  u3_noun wir = u3nt(c3__khan,
                     u3dc("scot", c3__uv, cop_u->sev_l),
                     u3_nul);
  u3_noun cad = u3nc(c3__born, u3_nul);

  u3_auto_peer(
    u3_auto_plan(car_u, u3_ovum_init(0, c3__k, wir, cad)),
    0,
    _khan_born_news,
    _khan_born_bail);
}

/* _khan_io_kick(): apply effects.
*/
static c3_o
_khan_io_kick(u3_auto* car_u, u3_noun wir, u3_noun cad)
{
  u3_khan* cop_u = (u3_khan*)car_u;

  u3_noun tag, dat, i_wir;
  c3_o ret_o;

  if (  (c3n == u3r_cell(wir, &i_wir, 0))
     || (c3n == u3r_cell(cad, &tag, &dat))
     || (c3__khan != i_wir) )
  {
    ret_o = c3n;
  }
  else {
    ret_o = c3y;
    // TODO do something
  }

  u3z(wir); u3z(cad);
  return ret_o;
}

/* _khan_io_exit(): unlink socket.
*/
static void
_khan_io_exit(u3_auto* car_u)
{
  u3_khan* cop_u = (u3_khan*)car_u;
  c3_c*    pax_c = u3_Host.dir_c;
  c3_w     len_w = strlen(pax_c) + 1 + sizeof(URB_SOCK_PATH);
  c3_c*    paf_c = c3_malloc(len_w);
  c3_i     wit_i;

  wit_i = snprintf(paf_c, len_w, "%s/%s", pax_c, URB_SOCK_PATH);
  c3_assert(wit_i > 0);
  c3_assert(len_w == (c3_w)wit_i + 1);

  unlink(paf_c);
  c3_free(paf_c);
}

/* u3_khan(): initialize control plane socket.
*/
u3_auto*
u3_khan_io_init(u3_pier* pir_u)
{
  u3_khan* cop_u = c3_calloc(sizeof(*cop_u));

  u3_auto* car_u = &cop_u->car_u;
  car_u->nam_m = c3__khan;
  car_u->io.talk_f = _khan_io_talk;
  car_u->io.kick_f = _khan_io_kick;
  car_u->io.exit_f = _khan_io_exit;

  {
    u3_noun now;
    struct timeval tim_u;
    gettimeofday(&tim_u, 0);

    now = u3_time_in_tv(&tim_u);
    cop_u->sev_l = u3r_mug(now);
    u3z(now);
  }

  return car_u;
}
