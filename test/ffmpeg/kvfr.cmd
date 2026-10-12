ffmpeg -i g12vfr2.mp4 -vf trim=start_frame=13:end_frame=51,setpts=PTS-STARTPTS -af atrim=start_sample=22932:end_sample=128772,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
