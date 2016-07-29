#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glut.h>

#define BUF_NUM 10
#define SIG_KILL 0x1000

char *video_dev;
int video_buf_len;
void *video_buffers[BUF_NUM];
int recv_sig;
int width = 640, height = 480;
const int bytes_per_pixel = 2;
uint32_t pixel_format = V4L2_PIX_FMT_YUYV;
//int pixel_format = V4L2_PIX_FMT_RGB565;

pthread_t stream_thread;
typedef void(frame_cb)(void *frame, void *user_ptr);

struct cb_handle {
	int fd;
	frame_cb *call_back;
};

SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *texture;
void *temp;

int v4l2_open(char *device)
{
	struct stat st;
	if (!device)
		return -1;
	if (-1 == stat(device, &st))
		return -1;
	if (!S_ISCHR(st.st_mode)) {
		printf("no char dev:%s\n", device);
		return -1;
	}
	printf("%s is a char dev\n", device);
	return open(device, O_RDWR | O_NONBLOCK, 0);
}

int v4l2_close(int fd)
{
	return close(fd);
}

int v4l2_querycap(int fd, struct v4l2_capability *cap)
{
	if (-1 == ioctl(fd, VIDIOC_QUERYCAP, cap))
		printf("VIDIOC_QUERYCAP failure");
	printf("driver:%s\n", cap->driver);
	printf("card:%s\n", cap->card);
	printf("bus:%s\n", cap->bus_info);
	printf("version:0x%x\n", cap->version);
	printf("capabilities:0x%x\n", cap->capabilities);
	printf("device_caps:0x%x\n", cap->device_caps);
	return 0;
}

int v4l2_sfmt(int fd, int width, int height, uint32_t pfmt)
{
	int ret;
	struct v4l2_cropcap cropcap;
	struct v4l2_crop crop;
	struct v4l2_format fmt;
	memset(&cropcap, 0, sizeof(cropcap));
	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (0 == ioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
		memset(&crop, 0, sizeof(crop));
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect;
		ioctl(fd, VIDIOC_S_CROP, &crop);
	}
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = pfmt;
	fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
	ret = ioctl(fd, VIDIOC_S_FMT, &fmt);
	printf("VIDIOC_S_FMT ret:%d type:%x width:%d\
		height:%d fmt:%x bytesperline:%d sizeimage:%d\n",
		ret, fmt.type,
		fmt.fmt.pix.width, fmt.fmt.pix.height,
		fmt.fmt.pix.pixelformat,
		fmt.fmt.pix.bytesperline,
		fmt.fmt.pix.sizeimage);
	return ret;
}

int v4l2_mmap(int fd, void *buffers[], int *buf_length)
{
	struct v4l2_requestbuffers req;
	int idx;
	memset(&req, 0, sizeof(req));
	req.count = BUF_NUM;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (-1 == ioctl(fd, VIDIOC_REQBUFS, &req)) {
		printf("VIDIOC_REQBUFS failure\n");
		return -1;
	}
	printf("VIDIOC_REQBUFS success\n");
	for (idx = 0; idx < req.count; idx++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = req.type;
		buf.memory = req.memory;
		buf.index = idx;
		if (-1 == ioctl(fd, VIDIOC_QUERYBUF, &buf)) {
			printf("VIDIOC_QUERYBUF failure\n");
			continue;
		}
		//printf("VIDIOC_QUERYBUF buf length:%d\n", buf.length);
		*buf_length = buf.length;
		buffers[idx] = mmap(NULL, buf.length,
			PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, buf.m.offset);
		if (MAP_FAILED == buffers[idx]) {
			printf("VIDIOC_QUERYBUF map failure\n");
			continue;
		}
		//printf("QUERYBUF & mmap success:buf%d:%p\n", idx, buffers[idx]);
	}
	return 0;
}

int v4l2_munmap(void *buffers[], int length)
{
	int idx;
	for (idx = 0; idx < BUF_NUM; idx++) {
		if (munmap(buffers[idx], length))
			printf("munmap failure:%p\n", buffers[idx]);
		//else
		//	printf("munmap success:%p\n", buffers[idx]);
	}
	return 0;
}

static void *stream_func(void *cb_handle)
{
	fd_set fds;
	struct timeval tv;
	struct v4l2_buffer buf;
	struct cb_handle *handle = (struct cb_handle *)cb_handle;
	printf("stream is on\n");

	if (SDL_Init(SDL_INIT_VIDEO) != 0){
		printf("SDL_Init Error\n");
		return NULL;
	}
	window = SDL_CreateWindow("sdl_viewer", 100, 100, width * 2, height, SDL_WINDOW_SHOWN);
	if (window == NULL) {
		printf("SDL_CreateWindow Error\n");
		return NULL;
	}
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (renderer == NULL) {
		printf("SDL_CreateRenderer Error\n");
		return NULL;
	}
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YUY2, SDL_TEXTUREACCESS_STREAMING, width * 2, height);

	while (!recv_sig) {
		int ret;
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		FD_ZERO(&fds);
		FD_SET(handle->fd, &fds);
		ret = select(handle->fd + 1, &fds, NULL, NULL, &tv);
		if (-1 == ret) {
			continue;
		} else if (0 == ret) {
			continue;
		}
		if (FD_ISSET(handle->fd, &fds)) {
			memset(&buf, 0, sizeof(buf));
			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			if (-1 == ioctl(handle->fd, VIDIOC_DQBUF, &buf)) {
				printf("VIDIOC_DQBUF failure\n");
				return NULL;
			}
			if (handle->call_back)
				(*handle->call_back)(video_buffers[buf.index], NULL);
			if (-1 == ioctl(handle->fd, VIDIOC_QBUF, &buf))
				printf("VIDIOC_QBUF failure\n");
		}
	}
	return NULL;
}

int v4l2_streamon(int fd, struct cb_handle *handle)
{
	enum v4l2_buf_type type;
	int idx;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	for (idx = 0; idx < BUF_NUM; idx++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = idx;
		if (-1 == ioctl(fd, VIDIOC_QBUF, &buf)) {
			printf("VIDIOC_QBUF failure\n");
			continue;
		}
		//printf("VIDIOC_QBUF success\n");
	}
	if (-1 == ioctl(fd, VIDIOC_STREAMON, &type)) {
		printf("VIOIOC_STREAMON failure\n");
		return -1;
	}
	handle->fd = fd;
	if (pthread_create(&stream_thread, NULL, stream_func, (void *)handle)) {
		printf("pthread_create failure\n");
		return -1;
	}
	return 0; 
}

int v4l2_streamoff(int fd)
{
	enum v4l2_buf_type type;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == ioctl(fd, VIDIOC_STREAMOFF, &type))
		printf("VIDIOC_STREAMOFF failure\n");
	recv_sig = 1;
	pthread_join(stream_thread, NULL);
	printf("stream is off\n");
	return 0;
}

void graytoycbcr(unsigned char* src, unsigned char* dst, int width, int height)
{
	int i;
	for (i = 0; i < width * height; i += 2) {
		float gray = *src++;
		*dst++ = (char)(0.299*gray + 0.587*gray + 0.114*gray);
		*dst++ = (char)(128.0 - 0.168636*gray - 0.331264*gray + 0.5*gray);
		gray = *src++;
		*dst++ = (char)(0.299*gray + 0.587*gray + 0.114*gray);
		*dst++ = (char)(128.0 + 0.5*gray - 0.418688*gray - 0.081312*gray);
	}
}

void callback_func(void *frame, void *user_ptr)
{
	char buffer[width * height * bytes_per_pixel * 2];
	//printf("frame %dx%d: %p\n", width, height, frame);
	SDL_Rect sourceRect = {0, 0, width * 2, height};
	SDL_Rect destRect = {0, 0, width * 4, height * 2};
	graytoycbcr(frame, buffer, width * 2, height);
	SDL_UpdateTexture(texture, &sourceRect, buffer, width * 2 * bytes_per_pixel);
	SDL_RenderCopy(renderer, texture, &sourceRect, &destRect);
	SDL_RenderPresent(renderer);
}

int main(int argc, char **argv)
{
	int errno;
	int video_fd;
	struct v4l2_capability cap;
	struct cb_handle handle;

	if (argc < 2) {
		printf("Usage: v4l2dsl width height (/dev/video#)\n");
		return -1;
	}
	video_dev = "/dev/video0";
	if (argc > 2) {
		width = atoi(argv[1]);
		height = atoi(argv[2]);
	}
	if (argc > 3) {
		video_dev = argv[3];
	}

	video_fd = v4l2_open(video_dev);
	if (video_fd == -1) {
		printf("%s open error\n", video_dev);
		exit(-1);
	}

	errno = v4l2_querycap(video_fd, &cap);
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		printf("V4L2_CAP_VIDEO_CAPTURE not supported\n");
		goto main_exit;
	}
	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
	 	printf("V4L2_CAP_STREAMING not supported\n");
		goto main_exit;
	}
	errno = v4l2_sfmt(video_fd, width, height, pixel_format);
	if (errno) {
		printf("VIDIOC_S_FMT failure\n");
		goto main_exit;
	}
	printf("VIDIOC_S_FMT success\n");
	errno = v4l2_mmap(video_fd, video_buffers, &video_buf_len);
	if (errno) {
		printf("v4l2 mmap failure\n");
		goto main_exit;
	}
	handle.fd = video_fd;
	handle.call_back = callback_func;

	errno = v4l2_streamon(video_fd, &handle);
	int quit = 0;
	SDL_Event e;
	while (!quit) {
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) {
				quit = 1;
			}
			if (e.type == SDL_KEYDOWN) {
			  if (e.key.keysym.sym == SDLK_ESCAPE)
				quit = 1;
			}
		}
	}
	errno = v4l2_streamoff(video_fd);	
	errno = v4l2_munmap(video_buffers, video_buf_len);
	if (errno) {
		printf("v4l2 munmap failure\n");
		goto main_exit;
	}
main_exit:
	errno = v4l2_close(video_fd);
	if (errno) {
		printf("close error %d\n", errno);
		exit(1);
	}
	SDL_Quit();
	return 0;
}
