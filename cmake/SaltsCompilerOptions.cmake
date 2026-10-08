include_guard(GLOBAL)

function(turbowasm_normalize_salts_c11_atomics)
  if(NOT MSVC)
    return()
  endif()

  # Published Salts SDKs (including 2.2.0) export the slash spelling.
  # sccache 0.18.0 treats that unknown option as an input file; MSVC accepts
  # the equivalent dash spelling. Preserve the SDK's C-only condition.
  foreach(_target IN ITEMS Salts::Platform Salts::Core)
    if(TARGET "${_target}")
      get_target_property(_options "${_target}" INTERFACE_COMPILE_OPTIONS)
      if(_options)
        list(TRANSFORM _options REPLACE
          "(^|:)/experimental:c11atomics(>|$)"
          "\\1-experimental:c11atomics\\2")
        set_property(TARGET "${_target}" PROPERTY
          INTERFACE_COMPILE_OPTIONS "${_options}")
      endif()
    endif()
  endforeach()
endfunction()
