# Host-library linkage rules: one GObject type system and one SQLite runtime.
# OS libraries are native host packages, never another distribution's sysroot.
# Installed with the SDK so that examples built against it use the same rules.
include_guard(GLOBAL)

# Replace static or sysroot GLib/GObject/GIO entries of a pkg-config imported
# target (for example PkgConfig::JSON for json-glib-1.0) with the host libraries.
function(amdva_use_host_glib target)
  pkg_get_variable(glib_libdir glib-2.0 libdir)
  get_target_property(links ${target} INTERFACE_LINK_LIBRARIES)
  foreach(component gio-2.0 gobject-2.0 glib-2.0)
    find_library(AMDVA_HOST_${component} NAMES ${component}
      PATHS "${glib_libdir}" /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH REQUIRED)
    list(FILTER links EXCLUDE REGEX "/lib${component}\\.(a|so)$")
    list(APPEND links "${AMDVA_HOST_${component}}")
  endforeach()
  set_target_properties(${target} PROPERTIES INTERFACE_LINK_LIBRARIES "${links}")
endfunction()

# Point a pkg-config sqlite3 imported target at the host shared library.
function(amdva_use_host_sqlite target)
  find_library(AMDVA_HOST_SQLITE NAMES sqlite3 libsqlite3.so.0
    PATHS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu NO_DEFAULT_PATH REQUIRED)
  set_target_properties(${target} PROPERTIES INTERFACE_LINK_LIBRARIES "${AMDVA_HOST_SQLITE}")
endfunction()

# find_library/find_path retain old cache values when an SDK root changes.
# Fail rather than linking an old .deps SDK behind a newly validated version.
function(amdva_require_sdk_path variable root)
  if(NOT EXISTS "${${variable}}")
    message(FATAL_ERROR "${variable} is missing; configure a fresh build directory")
  endif()
  file(REAL_PATH "${${variable}}" selected)
  file(REAL_PATH "${root}" sdk)
  string(FIND "${selected}/" "${sdk}/" position)
  if(NOT position EQUAL 0)
    message(FATAL_ERROR "${variable} resolves outside the selected SDK: ${selected}. Configure a fresh build directory.")
  endif()
endfunction()
