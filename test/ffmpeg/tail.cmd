ffmpeg -i av.mp4 -vf trim=start_frame=30:end_frame=50,setpts=PTS-STARTPTS -af atrim=start_sample=52920,asetpts=PTS-STARTPTS ref.mp4
