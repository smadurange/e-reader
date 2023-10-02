idf_component_register(SRCS "main.c" "epd.c" "util.c"
                       EMBED_TXTFILES git_srht_cert.pem
                       INCLUDE_DIRS ".")
