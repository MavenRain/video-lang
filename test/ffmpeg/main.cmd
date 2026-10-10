ffmpeg -i av.mp4 -vf trim=start_frame=10:end_frame=30,setpts=PTS-STARTPTS -af atrim=start_sample=17640:end_sample=52920,asetpts=PTS-STARTPTS ref.mp4
