function(remove_permissive TARGET)
    get_target_property(FLAGS ${TARGET} COMPILE_OPTIONS)
    if(FLAGS)
        string(REPLACE "/permissive-" "/permissive" OUT_FLAGS "${FLAGS}")
        set_property(TARGET ${TARGET} PROPERTY COMPILE_OPTIONS ${OUT_FLAGS})
    endif()
endfunction()
