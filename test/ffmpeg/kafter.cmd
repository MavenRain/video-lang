ffmpeg -i g12.mp4 -vf trim=start_frame=25:end_frame=41,setpts=PTS-STARTPTS -af atrim=start_sample=44100:end_sample=72324,asetpts=PTS-STARTPTS -fps_mode passthrough ref.mp4
