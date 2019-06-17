/*
 * Copyright 2017-2019 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.baidu.openrasp.tool.cpumonitor;

import com.baidu.openrasp.HookHandler;
import com.baidu.openrasp.cloud.CloudManager;
import com.baidu.openrasp.cloud.model.ErrorType;
import com.baidu.openrasp.cloud.utils.CloudUtils;
import com.baidu.openrasp.config.Config;

import java.io.*;
import java.lang.management.*;
import java.util.ArrayList;

/**
 * @description: rasp 监控java进程的cpu使用情况
 * @author: anyang
 * @create: 2019/06/03 15:38
 */
public class CpuMonitor {
    private static final String CPUS_ALLOWED_LIST = "Cpus_allowed_list";
    private static final String PROCESS_STATUS = "/proc/%d/status";
    private static final int SAMPLE_INTERVAL = 5;
    private static ArrayList<Float> cpuUsageList = new ArrayList<Float>(3);

    private boolean isAlive = true;
    private long lastTotalCpuTime;
    private long lastProcessCpuTime;

    CpuMonitor() {
        String pid = getPid();
        this.lastTotalCpuTime = System.currentTimeMillis();
        ProcCpuProcess processcpu = new ProcCpuProcess(pid);
        this.lastProcessCpuTime = processcpu.getProcessTotalCpuTime();
    }

    private float getCpuUsage() {
        float totalUsage = 0;
        try {
            String pid = getPid();
            long currentTotalCpuTime = System.currentTimeMillis();
            ProcCpuProcess processcpu = new ProcCpuProcess(pid);
            long currentProcessCpuTime = processcpu.getProcessTotalCpuTime();
            totalUsage = (float) (currentProcessCpuTime - this.lastProcessCpuTime) * 10 * 100 / (float) (currentTotalCpuTime - this.lastTotalCpuTime);
            this.lastTotalCpuTime = currentTotalCpuTime;
            this.lastProcessCpuTime = currentProcessCpuTime;
        } catch (Exception e) {
            String msg = "count cpu usage failed";
            int code = ErrorType.CPU_ERROR.getCode();
            HookHandler.LOGGER.warn(CloudUtils.getExceptionObject(msg, code), e);
        }
        return totalUsage;
    }

    private String getPid() {
        String name = ManagementFactory.getRuntimeMXBean().getName();
        return name.split("@")[0];
    }

    private float getCpuUsageUpper(String pid) {
        int totalCpuNum = 0;
        try {
            String path = PROCESS_STATUS.replace("%d", pid);
            BufferedReader reader = new BufferedReader(new InputStreamReader(new FileInputStream(new File(path))));
            String line = null;
            while ((line = reader.readLine()) != null) {
                if (line.startsWith(CPUS_ALLOWED_LIST)) {
                    line = line.trim();
                    String[] temp = line.split("\\s+");
                    for (String s : temp[1].split(",")) {
                        if (s.contains("-")) {
                            String[] num = s.split("-");
                            totalCpuNum += (Integer.parseInt(num[1]) - Integer.parseInt(num[0]) + 1);
                        } else {
                            totalCpuNum++;
                        }
                    }
                    break;
                }
            }
        } catch (Exception e) {
            String msg = "get server occupied cpu number failed";
            int code = ErrorType.CPU_ERROR.getCode();
            HookHandler.LOGGER.warn(CloudUtils.getExceptionObject(msg, code), e);
        }
        return totalCpuNum * 100 * Config.getConfig().getCpuUsagePercent();
    }

    private void checkCpuUsage() {
        float totalCpuUsage = getCpuUsage();
        float cpuUsageUpper = getCpuUsageUpper(getPid());
        if (totalCpuUsage > cpuUsageUpper) {
            if (!Config.getConfig().getDisableHooks()) {
                cpuUsageList.add(totalCpuUsage);
            }
            if (cpuUsageList.size() >= 3) {
                Config.getConfig().setDisableHooks("true");
            }
        } else {
            Config.getConfig().setDisableHooks("false");
            cpuUsageList.clear();
        }
    }

    public void start() {
        Thread thread = new Thread(new CpuMonitorThread());
        thread.setDaemon(true);
        thread.start();
    }

    public void stop() {
        this.isAlive = false;
    }

    class CpuMonitorThread implements Runnable {
        @Override
        public void run() {
            while (isAlive) {
                try {
                    Thread.sleep(SAMPLE_INTERVAL * 1000);
                    checkCpuUsage();
                } catch (Throwable e) {
                    String message = e.getMessage();
                    int errorCode = ErrorType.CPU_ERROR.getCode();
                    CloudManager.LOGGER.warn(CloudUtils.getExceptionObject(message, errorCode), e);
                }
            }
        }
    }
}
