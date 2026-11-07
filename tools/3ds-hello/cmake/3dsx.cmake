find_program(_smdhtool NAMES smdhtool HINTS "${DEVKITPRO}/tools/bin")
find_program(_3dsxtool NAMES 3dsxtool HINTS "${DEVKITPRO}/tools/bin")
if(NOT _smdhtool OR NOT _3dsxtool)
  message(FATAL_ERROR "smdhtool/3dsxtool not found under ${DEVKITPRO}/tools/bin")
endif()

function(eng_add_3dsx target)
  cmake_parse_arguments(A "" "TITLE;DESC;AUTHOR" "" ${ARGN})
  if(NOT A_TITLE)
    set(A_TITLE "${target}")
  endif()
  if(NOT A_DESC)
    set(A_DESC "PocketLoom")
  endif()
  if(NOT A_AUTHOR)
    set(A_AUTHOR "PocketLoom")
  endif()
  set(_outdir "$<TARGET_FILE_DIR:${target}>")
  set(_smdh "${_outdir}/${target}.smdh")
  set(_3dsx "${_outdir}/${target}.3dsx")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${_smdhtool}" --create "${A_TITLE}" "${A_DESC}" "${A_AUTHOR}" "${CTRULIB}/default_icon.png" "${_smdh}"
    COMMAND "${_3dsxtool}" "$<TARGET_FILE:${target}>" "${_3dsx}" "--smdh=${_smdh}"
    COMMENT "3dsx: packing ${target}.3dsx"
    VERBATIM)
endfunction()
