# SRS(Simple Realtime Server)

![](http://ossrs.net/gif/v1/sls.gif?site=github.com&path=/srs/develop)
[![](https://github.com/ossrs/srs/actions/workflows/codeql-analysis.yml/badge.svg?branch=develop)](https://github.com/ossrs/srs/actions?query=workflow%3ACodeQL+branch%3Adevelop)
[![](https://github.com/ossrs/srs/actions/workflows/release.yml/badge.svg)](https://github.com/ossrs/srs/actions/workflows/release.yml?query=workflow%3ARelease)
[![](https://img.shields.io/twitter/follow/srs_server?style=social)](https://twitter.com/srs_server)
[![](https://img.shields.io/badge/SRS-YouTube-red)](https://www.youtube.com/@srs_server)
[![](https://badgen.net/discord/members/yZ4BnPmHAd)](https://discord.gg/yZ4BnPmHAd)
[![](https://opencollective.com/srs-server/tiers/badge.svg)](https://opencollective.com/srs-server)
[![](https://img.shields.io/docker/pulls/ossrs/srs)](https://hub.docker.com/r/ossrs/srs/tags)
[![](https://codecov.io/gh/ossrs/srs/graph/badge.svg?token=Zx2LhdtA39)](https://codecov.io/gh/ossrs/srs)

SRS/7.0 ([Kai](https://ossrs.io/lts/en-us/product#release-70)) is a simple, high-efficiency, and real-time video server, 
supporting RTMP/WebRTC/HLS/HTTP-FLV/SRT/MPEG-DASH/GB28181, Linux/macOS, X86_64/ARMv7/AARCH64/M1/RISCV/LOONGARCH/MIPS, 
and essential [features](trunk/doc/Features.md#features).

[![SRS Overview](https://ossrs.net/wiki/images/SRS-SingleNode-4.0-sd.png?v=114)](https://ossrs.net/wiki/images/SRS-SingleNode-4.0-hd.png)

> Note: For more details on the single-node architecture for SRS, please visit the following [link](https://www.figma.com/file/333POxVznQ8Wz1Rxlppn36/SRS-4.0-Server-Arch).

SRS is licenced under [MIT](https://github.com/ossrs/srs/blob/develop/LICENSE), and some third-party libraries are 
distributed under their [licenses](https://ossrs.io/lts/en-us/license).

<a name="product"></a> <a name="usage-docker"></a>

## Usage

Please check the Getting Started guide in [English](https://ossrs.io/lts/en-us/docs/v5/doc/getting-started) 
or [Chinese](https://ossrs.net/lts/zh-cn/docs/v5/doc/getting-started). We highly recommend using SRS with docker:

```bash
docker run --rm -it -p 1935:1935 -p 1985:1985 -p 8080:8080 \
    -p 8000:8000/udp -p 10080:10080/udp ossrs/srs:6
```

> Tips: If you're in China, use this image `registry.cn-hangzhou.aliyuncs.com/ossrs/srs:6` for faster speed.

Open [http://localhost:8080/](http://localhost:8080/) to verify, and then stream using the following
[FFmpeg](https://ffmpeg.org/download.html) command:

```bash
ffmpeg -re -i ./doc/source.flv -c copy -f flv -y rtmp://localhost/live/livestream
```

Alternatively, stream by [OBS](https://obsproject.com/download) using the following configuration:

* Service: `Custom`
* Server: `rtmp://localhost/live`
* Stream Key: `livestream`

Play the following streams using media players:

* To play an RTMP stream with URL `rtmp://localhost/live/livestream` on [VLC player](https://www.videolan.org/), open the player, go to Media > Open Network Stream, enter the URL and click Play.
* You can play HTTP-FLV stream URL [http://localhost:8080/live/livestream.flv](http://localhost:8080/players/srs_player.html?autostart=true&stream=livestream.flv) on a webpage using the srs-player, an HTML5-based player.
* Use srs-player for playing HLS stream with URL [http://localhost:8080/live/livestream.m3u8](http://localhost:8080/players/srs_player.html?autostart=true&stream=livestream.m3u8).

If you'd like to use WebRTC, convert RTMP to WebRTC, or convert WebRTC to RTMP, please check out 
the wiki documentation in either [English](https://ossrs.io/lts/en-us/docs/v5/doc/getting-started#webrtc) or 
[Chinese](https://ossrs.net/lts/zh-cn/docs/v5/doc/getting-started#webrtc).

To learn more about RTMP, HLS, HTTP-FLV, SRT, MPEG-DASH, WebRTC protocols, clustering, 
HTTP API, DVR, and transcoding, please check the documents in [English](https://ossrs.io) 
or [Chinese](https://ossrs.net).

If you want to use an IDE, VSCode is recommanded. VSCode supports macOS, and Linux 
platforms. The settings are ready. All you need to do is open the folder with VSCode and 
enjoy the efficiency brought by the IDE. See [VSCode README](.vscode/README.md) for details.



