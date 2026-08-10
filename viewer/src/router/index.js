import { createRouter, createWebHashHistory, createWebHistory } from "vue-router";
import LogAnalyzer from "../views/LogAnalyzer.vue";
import DeviceConfig from "../views/DeviceConfig.vue";

const routes = [
  { path: "/", name: "viewer", component: LogAnalyzer },
  { path: "/device", name: "device", component: DeviceConfig },
];

const single = import.meta.env.MODE === "single";
const baseUrl = import.meta.env.PROD ? import.meta.env.BASE_URL : "";

export default createRouter({
  history: single ? createWebHashHistory(baseUrl) : createWebHistory(baseUrl),
  routes,
});
