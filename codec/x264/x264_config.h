#define X264_BIT_DEPTH     8
#define X264_GPL           1
#define X264_INTERLACED    0
#define X264_CHROMA_FORMAT 0
#define X264_VERSION ""
#define X264_POINTVER "0.142.x"

#define BIT_DEPTH X264_BIT_DEPTH
#define HAVE_INTERLACED 1
#define HAVE_THREAD 1
#define HAVE_GPL 1
#define HAVE_STRING_H 1
#define HAVE_BITDEPTH8 1

#ifdef _WIN32

#define SYS_WINDOWS 1
#define HAVE_WIN32THREAD 1

#else

#define HAVE_POSIXTHREAD 1

#endif
