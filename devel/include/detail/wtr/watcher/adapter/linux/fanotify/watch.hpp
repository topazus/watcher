#pragma once

#if (defined(__linux__) || defined(__ANDROID_API__)) \
  && ! defined(WATER_WATCHER_USE_WARTHOG)

#include <linux/version.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)) \
  && ! defined(__ANDROID_API__)

#include "wtr/watcher.hpp"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <sys/epoll.h>
#include <sys/fanotify.h>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace detail {
namespace wtr {
namespace watcher {
namespace adapter {
namespace fanotify {

/*  - delay
      The delay, in milliseconds, while `epoll_wait` will
      'sleep' for until we are woken up. We usually check
      if we're still alive at that point.
    - event_wait_queue_max
      Number of events allowed to be given to recv
      (returned by `epoll_wait`). Any number between 1
      and some large number should be fine. We don't
      lose events if we 'miss' them, the events are
      still waiting in the next call to `epoll_wait`.
    - event_buf_len:
      For our event buffer, 4096 is a typical page size
      and sufficiently large to hold a great many events.
      That's a good thumb-rule, and it might be the best
      value to use because there will be a possibly long
      character string (for the filename) in the event.
      We can infer some things about the size we need for
      the event buffer, but it's unlikely to be meaningful
      because of the variably sized character string being
      reported. We could use something like:
          event_buf_len
            = ((event_wait_queue_max + PATH_MAX)
            * (3 * sizeof(fanotify_event_metadata)));
      But that's a lot of flourish for 72 bytes that won't
      be meaningful.
    - fan_init_flags:
      Post-event reporting, non-blocking IO and unlimited
      marks. We need sudo mode for the unlimited marks.
      If we were making a filesystem auditor, we might use:
          FAN_CLASS_PRE_CONTENT
          | FAN_UNLIMITED_QUEUE
          | FAN_UNLIMITED_MARKS
    - fan_init_opt_flags:
      Read-only, non-blocking, and close-on-exec. */
inline constexpr auto delay_ms = 16;
inline constexpr auto event_wait_queue_max = 1;
inline constexpr auto event_buf_len = PATH_MAX;
inline constexpr auto fan_init_flags = FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME
                                     | FAN_UNLIMITED_QUEUE
                                     | FAN_UNLIMITED_MARKS;
inline constexpr auto fan_init_opt_flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC;

/*  - mark_set_type
        A set of file descriptors for fanotify resources.
    - system_resources
        An object holding:
          - An fanotify file descriptor
          - An epoll file descriptor
          - An epoll configuration
          - A set of watch marks (as returned by fanotify_mark)
          - A map of (sub)path handles to filesystem paths (names)
          - A boolean: whether or not the resources are valid */
using mark_set_type = std::unordered_set<int>;

struct system_resources {
  bool valid;
  int watch_fd;
  int event_fd;
  epoll_event event_conf;
  mark_set_type mark_set;
};

inline auto mark(
  std::filesystem::path const& full_path,
  int watch_fd,
  mark_set_type& ms) noexcept -> bool
{
  int wd = fanotify_mark(
    watch_fd,
    FAN_MARK_ADD,
    FAN_ONDIR | FAN_CREATE | FAN_MODIFY | FAN_DELETE | FAN_MOVE
      | FAN_DELETE_SELF | FAN_MOVE_SELF,
    AT_FDCWD,
    full_path.c_str());
  if (wd >= 0) {
    ms.insert(wd);
    return true;
  }
  else
    return false;
};

inline auto
mark(std::filesystem::path const& full_path, system_resources& sr) noexcept
  -> bool
{
  return mark(full_path, sr.watch_fd, sr.mark_set);
};

inline auto unmark(
  std::filesystem::path const& full_path,
  int watch_fd,
  mark_set_type& mark_set) noexcept -> bool
{
  int wd = fanotify_mark(
    watch_fd,
    FAN_MARK_REMOVE,
    FAN_ONDIR | FAN_CREATE | FAN_MODIFY | FAN_DELETE | FAN_MOVE
      | FAN_DELETE_SELF | FAN_MOVE_SELF,
    AT_FDCWD,
    full_path.c_str());
  auto const& at = mark_set.find(wd);

  if (wd >= 0 && at != mark_set.end()) {
    mark_set.erase(at);
    return true;
  }
  else
    return false;
};

inline auto
unmark(std::filesystem::path const& full_path, system_resources& sr) noexcept
  -> bool
{
  return unmark(full_path, sr.watch_fd, sr.mark_set);
};

/*  Produces a `system_resources` with the file descriptors from
    `fanotify_init` and `epoll_create`. Invokes `callback` on errors. */
inline auto open_system_resources(
  std::filesystem::path const& path,
  ::wtr::watcher::event::callback const& callback) noexcept -> system_resources
{
  namespace fs = std::filesystem;
  using ev = ::wtr::watcher::event;

  auto do_error = [&path, &callback](
                    char const* const msg,
                    int watch_fd,
                    int event_fd = -1) noexcept -> system_resources
  {
    callback(
      {std::string{msg} + "(" + std::strerror(errno) + ")@" + path.string(),
       ev::effect_type::other,
       ev::path_type::watcher});

    return system_resources{
      .valid = false,
      .watch_fd = watch_fd,
      .event_fd = event_fd,
      .event_conf = {.events = 0, .data = {.fd = watch_fd}},
      .mark_set = {},
    };
  };

  auto do_path_map_container_create =
    [](
      int const watch_fd,
      fs::path const& base_path,
      ::wtr::watcher::event::callback const& callback) -> mark_set_type
  {
    using diter = fs::recursive_directory_iterator;

    static constexpr auto dopt =
      fs::directory_options::skip_permission_denied
      & fs::directory_options::follow_directory_symlink;

    static constexpr auto rsrv_count = 1024;

    auto pmc = mark_set_type{};
    pmc.reserve(rsrv_count);

    try {
      if (mark(base_path, watch_fd, pmc))
        if (fs::is_directory(base_path))
          for (auto& dir : diter(base_path, dopt))
            if (fs::is_directory(dir))
              if (! mark(dir.path(), watch_fd, pmc))
                callback(
                  {"w/sys/not_watched@" + base_path.string() + "@"
                     + dir.path().string(),
                   ev::effect_type::other,
                   ev::path_type::watcher});
    } catch (...) {}

    return pmc;
  };

  int watch_fd = fanotify_init(fan_init_flags, fan_init_opt_flags);
  if (watch_fd >= 0) {
    auto pmc = do_path_map_container_create(watch_fd, path, callback);
    if (! pmc.empty()) {
      epoll_event event_conf{.events = EPOLLIN, .data{.fd = watch_fd}};

      int event_fd = epoll_create1(EPOLL_CLOEXEC);

      /*  @note We could make the epoll and fanotify file
          descriptors non-blocking with `fcntl`. It's not
          clear if we can do this from their `*_init` calls.

          fcntl(watch_fd, F_SETFL, O_NONBLOCK);
          fcntl(event_fd, F_SETFL, O_NONBLOCK); */

      if (event_fd >= 0)
        if (epoll_ctl(event_fd, EPOLL_CTL_ADD, watch_fd, &event_conf) >= 0)
          return system_resources{
            .valid = true,
            .watch_fd = watch_fd,
            .event_fd = event_fd,
            .event_conf = event_conf,
            .mark_set = std::move(pmc),
          };
        else
          return do_error("e/sys/epoll_ctl", watch_fd, event_fd);
      else
        return do_error("e/sys/epoll_create", watch_fd, event_fd);
    }
    else
      return do_error("e/sys/fanotify_mark", watch_fd);
  }
  else
    return do_error("e/sys/fanotify_init", watch_fd);
};

inline auto close_system_resources(system_resources&& sr) noexcept -> bool
{
  return close(sr.watch_fd) == 0 && close(sr.event_fd) == 0;
};

/*  "Promotes" an event's metadata to a full path.
    The shenanigans we do here depend on this event being
    `FAN_EVENT_INFO_TYPE_DFID_NAME`. The kernel passes us
    some info about the directory and the directory entry
    (the filename) of this event that doesn't exist when
    other event types are reported. In particular, we need
    a file descriptor to the directory (which we use
    `readlink` on) and a character string representing the
    name of the directory entry.
    TLDR: We need information for the full path of the
    event, information which is only reported inside this
    `if`.
    From the kernel:
    Variable size struct for
    dir file handle + child file handle + name
    [ Omitting definition of `fanotify_info` here ]
    (struct fanotify_fh)
      dir_fh starts at
        buf[0] (optional)
      dir2_fh starts at
        buf[dir_fh_totlen] (optional)
      file_fh starts at
        buf[dir_fh_totlen+dir2_fh_totlen]
      name starts at
        buf[dir_fh_totlen+dir2_fh_totlen+file_fh_totlen]
      ...
    The kernel guarentees that there is a null-terminated
    character string to the event's directory entry
    after the file handle to the directory.
    Confusing, right? */
// clang-format off
/*  note at the end of file re. clang format */
inline auto promote(fanotify_event_metadata const* mtd) noexcept
-> std::tuple<
  bool,
  std::filesystem::path,
  enum ::wtr::watcher::event::effect_type,
  enum ::wtr::watcher::event::path_type>
{
  namespace fs = std::filesystem;
  using ev = ::wtr::watcher::event;

  auto path_imbue = [](
                      char* path_accum,
                      fanotify_event_info_fid const* dfid_info,
                      file_handle* dir_fh,
                      ssize_t dir_name_len = 0) noexcept -> void
  {
    char* name_info = (char*)(dfid_info + 1);
    char* file_name = static_cast<char*>(
      name_info + sizeof(file_handle) + sizeof(dir_fh->f_handle)
      + sizeof(dir_fh->handle_bytes) + sizeof(dir_fh->handle_type));

    if (file_name && std::strcmp(file_name, ".") != 0)
      std::snprintf(
        path_accum + dir_name_len,
        PATH_MAX - dir_name_len,
        "/%s",
        file_name);
  };

  auto dir_fid_info = ((fanotify_event_info_fid const*)(mtd + 1));

  auto dir_fh = (file_handle*)(dir_fid_info->handle);

  auto effect_type = mtd->mask & FAN_CREATE ? ev::effect_type::create
            : mtd->mask & FAN_DELETE ? ev::effect_type::destroy
            : mtd->mask & FAN_MODIFY ? ev::effect_type::modify
            : mtd->mask & FAN_MOVE   ? ev::effect_type::rename
                                     : ev::effect_type::other;

  auto path_type = mtd->mask & FAN_ONDIR ? ev::path_type::dir : ev::path_type::file;

  /*  We can get a path name, so get that and use it */
  char path_buf[PATH_MAX];
  int fd = open_by_handle_at(
    AT_FDCWD,
    dir_fh,
    O_RDONLY | O_CLOEXEC | O_PATH | O_NONBLOCK);
  if (fd > 0){
    char fs_proc_path[128];
    std::snprintf(fs_proc_path, sizeof(fs_proc_path), "/proc/self/fd/%d", fd);
    ssize_t dirname_len =
      readlink(fs_proc_path, path_buf, sizeof(path_buf) - sizeof('\0'));
    close(fd);

    if (dirname_len > 0){
      /*  Put the directory name in the path accumulator.
          Passing `dirname_len` has the effect of putting
          the event's filename in the path buffer as well. */
      path_buf[dirname_len] = '\0';
      path_imbue(path_buf, dir_fid_info, dir_fh, dirname_len);

      return std::make_tuple(true, fs::path{std::move(path_buf)}, effect_type, path_type);}

    else
      return std::make_tuple(false, fs::path{}, effect_type, path_type);
  }

  else {
    path_imbue(path_buf, dir_fid_info, dir_fh);

    return std::make_tuple(true, fs::path{std::move(path_buf)}, effect_type, path_type);
  }
};

// clang-format on

inline auto check_and_update(
  std::tuple<
    bool,
    std::filesystem::path,
    enum ::wtr::watcher::event::effect_type,
    enum ::wtr::watcher::event::path_type> const& r,
  system_resources& sr) noexcept
  -> std::tuple<
    bool,
    std::filesystem::path,
    enum ::wtr::watcher::event::effect_type,
    enum ::wtr::watcher::event::path_type> {
    using ev = ::wtr::watcher::event;

    auto [valid, path, effect_type, path_type] = r;

    return std::make_tuple(

      valid

        ? path_type == ev::path_type::dir

          ? effect_type == ev::effect_type::create  ? mark(path, sr)
          : effect_type == ev::effect_type::destroy ? unmark(path, sr)
                                                    : true

          : true

        : false,

      path,

      effect_type,

      path_type);
  };

/*  Send events to the user.
    This is the important part.
    Most of the other code is
    a layer of translation
    between us and the kernel. */
inline auto send(
  std::tuple<
    bool,
    std::filesystem::path,
    enum ::wtr::watcher::event::effect_type,
    enum ::wtr::watcher::event::path_type> const& from_kernel,
  ::wtr::watcher::event::callback const& callback) noexcept -> bool
{
  auto [ok, path, effect_type, path_type] = from_kernel;

  return ok ? (callback({path, effect_type, path_type}), ok) : ok;
};

/*  Reads through available (fanotify) filesystem events.
    Discerns their path and type.
    Calls the callback.
    Returns false on eventful errors.
    @note
    The `metadata->fd` field contains either a file
    descriptor or the value `FAN_NOFD`. File descriptors
    are always greater than 0. `FAN_NOFD` represents an
    event queue overflow for `fanotify` listeners which
    are _not_ monitoring file handles, such as mount
    monitors. The file handle is in the metadata when an
    `fanotify` listener is monitoring events by their
    file handles.
    The `metadata->vers` field may differ between kernel
    versions, so we check it against the version we were
    compiled with. */
inline auto recv(
  system_resources& sr,
  std::filesystem::path const& base_path,
  ::wtr::watcher::event::callback const& callback) noexcept -> bool
{
  /*  Read some events into a buffer.
      Send valid events to the user's callback.
      Send a diagnostic to the user on warnings and errors.
      Return false on errors. True otherwise. */

  using event_metadata = fanotify_event_metadata;
  using event_info_fid = fanotify_event_info_fid;

  static constexpr auto event_count_upper_lim =
    event_buf_len / sizeof(fanotify_event_metadata);

  auto do_error = [&base_path,
                   &callback](char const* const msg) noexcept -> bool
  {
    return (
      callback(
        {std::string{msg} + "@" + base_path.string(),
         ::wtr::watcher::event::effect_type::other,
         ::wtr::watcher::event::path_type::watcher}),
      false);
  };

  unsigned event_count = 0;
  alignas(fanotify_event_metadata) char event_buf[event_buf_len];
  auto read_len = read(sr.watch_fd, event_buf, sizeof(event_buf));
  enum class read_state {
    eventful,
    eventless,
    e_read
  } read_state = read_len > 0    ? read_state::eventful
               : read_len == 0   ? read_state::eventless
               : errno == EAGAIN ? read_state::eventless
                                 : read_state::e_read;

  switch (read_state) {
    case read_state::eventful :
      for (auto* mtd = (event_metadata*)(event_buf);
           FAN_EVENT_OK(mtd, read_len);
           mtd = FAN_EVENT_NEXT(mtd, read_len)) {
        auto hifty = ((event_info_fid const*)(mtd + 1))->hdr.info_type;
        enum class event_state {
          eventful,
          e_version,
          e_count,
          w_fd,
          w_q_overflow,
          w_info_type
        } event_state =
          mtd->vers != FANOTIFY_METADATA_VERSION   ? event_state::e_version
          : event_count++ > event_count_upper_lim  ? event_state::e_count
          : mtd->fd != FAN_NOFD                    ? event_state::w_fd
          : mtd->mask & FAN_Q_OVERFLOW             ? event_state::w_q_overflow
          : hifty != FAN_EVENT_INFO_TYPE_DFID_NAME ? event_state::w_info_type
                                                   : event_state::eventful;
        switch (event_state) {
          case event_state::eventful :
            send(check_and_update(promote(mtd), sr), callback);
            break;
          case event_state::e_version : return do_error("e/sys/kernel_version");
          case event_state::e_count : return do_error("e/sys/bad_count");
          case event_state::w_fd : return ! do_error("w/sys/bad_fd");
          case event_state::w_q_overflow : return ! do_error("w/sys/overflow");
          case event_state::w_info_type : return ! do_error("w/sys/bad_info");
        }
      }
      return true;

    case read_state::eventless : return true;

    case read_state::e_read : return do_error("e/sys/read");
  }

  assert(! "Unreachable");
  return false;
};

inline auto watch(
  std::filesystem::path const& path,
  ::wtr::watcher::event::callback const& callback,
  std::atomic<bool>& is_living) noexcept -> bool
{
  using ev = ::wtr::watcher::event;

  auto do_error = [&path, &callback](auto& f, char const* const msg) -> bool
  {
    return (
      callback(
        {std::string{msg} + "@" + path.string(),
         ev::effect_type::other,
         ev::path_type::watcher}),

      f(),

      false);
  };

  /*  While:
      - System resources for fanotify and epoll
      - An event list for receiving epoll events
      - We're alive
      Do:
      - Await filesystem events
      - Invoke `callback` on errors and events */

  auto sr = open_system_resources(path, callback);

  auto close = [&sr]() -> bool
  { return close_system_resources(std::move(sr)); };

  epoll_event event_recv_list[event_wait_queue_max];

  if (sr.valid) [[likely]] {
    while (is_living) [[likely]]

    {
      int event_count = epoll_wait(
        sr.event_fd,
        event_recv_list,
        event_wait_queue_max,
        delay_ms);
      if (event_count < 0)
        return do_error(close, "e/sys/epoll_wait");

      else if (event_count > 0) [[likely]]
        for (int n = 0; n < event_count; n++)
          if (event_recv_list[n].data.fd == sr.watch_fd) [[likely]]
            if (is_living) [[likely]]
              if (! recv(sr, path, callback)) [[unlikely]]
                return do_error(close, "e/self/event_recv");
    }

    return close();
  }

  else
    return do_error(close, "e/self/sys_resource");
};

// clang-format off
/*  returning tuples is confusing clang format */

} /*  namespace fanotify */
} /*  namespace adapter */
} /*  namespace watcher */
} /*  namespace wtr */
} /*  namespace detail */

// clang-format on

#endif
#endif
