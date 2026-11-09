# Legacy / PoolModes

Pool 방식을 AliveIndirectWave 하나로 합치기 전(커밋 f8f9c8a)의 파일 보관용 폴더.
빌드에는 포함되지 않음 (MyProject.vcxproj에 등록되어 있지 않음).

- Deleted/  : 통합 과정에서 삭제된 파일 (FullScan / SlotReuse / Compaction / AliveList Pool + 전용 셰이더)
- Replaced/ : 내용이 교체된 파일의 변경 전 버전 (ParticlePool, ParticleTypes, ParticleSystem, ParticleSettings, Main, vcxproj 등)

원본과 바이트 단위로 동일 (CP949 / CRLF 그대로).
다시 쓰려면 MyProject/ 로 복사한 뒤 vcxproj에 등록해야 함.
