ffmpeg -i av.mp4 -vf scale=240:180,scale=160:120,setpts=PTS-STARTPTS -af asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
