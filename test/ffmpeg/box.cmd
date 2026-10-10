ffmpeg -i av.mp4 -vf trim=start_frame=10:end_frame=30,setpts=PTS-STARTPTS,crop=160:120:40:20 -af atrim=start_sample=17640:end_sample=52920,asetpts=PTS-STARTPTS ref.mp4
