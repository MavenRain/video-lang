ffmpeg -i av.mp4 -vf setpts=PTS-STARTPTS -af asetpts=PTS-STARTPTS ref.mp4
