# ���ô���Ŀ���Զ�����ͷ�ļ�Ŀ¼
set(MyInclude1 "${PROJECT_SOURCE_DIR}/include")

################################################ �Զ���� ################################################

# ����һ���Զ����
add_library(LogMe "logme.c")

add_library(VList "vlist.c")

add_library(VUtils "vutils.c")

# �������� windows ƽ̨
add_library(TCPServer "tcpserver.c")

########################################### �Զ�����ͷ�ļ�Ŀ¼ ###########################################

# ʹ���Զ������Ҫ����ͷ�ļ�Ŀ¼��PUBLIC �����Ȿ���Ϳ��ʹ���߶���Ҫ����ͷ�ļ�Ŀ¼Ѱ��ͷ�ļ�
target_include_directories(LogMe PUBLIC ${MyInclude1})

target_include_directories(VList PUBLIC ${MyInclude1})

target_include_directories(VUtils PUBLIC ${MyInclude1})

# �������� windows ƽ̨
target_include_directories(TCPServer PUBLIC ${MyInclude1})

######################################### �Զ������Ҫ���ӵ������� #########################################

# һЩ�Զ������Ҫ���ӱ�Ŀ⣬PRIVATE �������ӵĿ�������Զ���Ȿ��

# �������� windows ƽ̨
target_link_libraries(TCPServer PRIVATE LogMe Ws2_32 VUtils)

##########################################################################################################