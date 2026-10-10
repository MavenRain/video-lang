ffmpeg -i av.mp4 -vf scale=640:480,setpts=PTS-STARTPTS -af asetpts=PTS-STARTPTS ref.mp4
